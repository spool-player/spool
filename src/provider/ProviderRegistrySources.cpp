#include "ProviderRegistry.h"

#include "ProviderPackage.h"
#include "ScriptRuntime.h"
#include "cache/DatabaseManager.h"

#include <QCoroNetworkReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <stdexcept>

namespace JellyfinNative {
namespace {
    const QString stateKey = QStringLiteral("portable/sources/1");

    bool validModule(const QString& id)
    {
        static const QRegularExpression pattern(QStringLiteral("^[a-z][a-z0-9]*(?:[.-][a-z0-9]+)+$"));
        return id.size() <= 128 && pattern.match(id).hasMatch();
    }

    bool boundedIdentity(const QString& value)
    {
        return !value.isEmpty() && value.size() <= 256;
    }
} // namespace

struct ProviderRegistry::PortableState {
    struct Source {
        QString id;
        QString module;
        QString account;
        QString key;
        QString label;
        bool enabled = true;
        bool configured = false;
        bool active = false;
        quint64 generation = 0;
        QVariantMap configuration;
        QList<QUrl> origins;
    };

    Source *find(const QString& id)
    {
        const auto found
            = std::find_if(sources.begin(), sources.end(), [&id](const Source& source) { return source.id == id; });
        return found == sources.end() ? nullptr : &*found;
    }

    void requireReady() const
    {
        if (!restored || !database)
            throw std::runtime_error("source_registry_not_ready");
        if (mutating)
            throw std::runtime_error("source_registry_busy");
    }

    QPointer<DatabaseManager> database;
    QPointer<QNetworkAccessManager> network;
    QString providersDirectory;
    bool isInstalling = false;
    QString installStatus;
    int installProgress = 0;
    QString installError;

    QHash<QString, ScriptRuntime *> modules;
    QList<Source> sources;
    QVariantList snapshot;
    bool restored = false;
    bool mutating = false;
};

ProviderRegistry::ProviderRegistry(QObject *parent)
    : QObject(parent)
    , m_portable(std::make_unique<PortableState>())
{
}

ProviderRegistry::~ProviderRegistry() = default;

void ProviderRegistry::registerModule(const QString& moduleId, const QString& entryPoint)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (!validModule(moduleId) || entryPoint.isEmpty() || m_portable->modules.contains(moduleId)
        || m_portable->modules.size() >= 4)
        throw std::runtime_error("invalid_or_duplicate_module");
    m_portable->modules.insert(moduleId, new ScriptRuntime(entryPoint, this));
}

QCoro::Task<void> ProviderRegistry::restoreSources(DatabaseManager *database)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (!database || m_portable->restored || m_portable->mutating)
        throw std::runtime_error("source_registry_already_initialized");
    QPointer<ProviderRegistry> guard(this);
    m_portable->mutating = true;
    const auto unlock = qScopeGuard([guard] {
        if (guard)
            guard->m_portable->mutating = false;
    });
    const QString stored = co_await database->loadSettingAsync(stateKey);
    if (!guard)
        throw std::runtime_error("source_registry_removed");
    if (stored.size() > 128 * 1024)
        throw std::runtime_error("source_state_limit");
    QList<PortableState::Source> restored;
    if (!stored.isEmpty()) {
        const QJsonDocument document = QJsonDocument::fromJson(stored.toUtf8());
        const QJsonObject root = document.object();
        if (!document.isObject() || root.value(QStringLiteral("format")).toInt() != 1
            || !root.value(QStringLiteral("sources")).isArray())
            throw std::runtime_error("incompatible_source_state");
        const QJsonArray records = root.value(QStringLiteral("sources")).toArray();
        if (records.size() > 64)
            throw std::runtime_error("source_state_limit");
        for (const QJsonValue& value : records) {
            const QJsonObject row = value.toObject();
            PortableState::Source source;
            source.id = row.value(QStringLiteral("id")).toString();
            source.module = row.value(QStringLiteral("module")).toString();
            source.account = row.value(QStringLiteral("account")).toString();
            source.key = row.value(QStringLiteral("key")).toString();
            source.label = row.value(QStringLiteral("label")).toString();
            source.enabled = row.value(QStringLiteral("enabled")).toBool();
            if (QUuid(source.id).isNull() || !validModule(source.module) || !boundedIdentity(source.account)
                || !boundedIdentity(source.key) || source.label.size() > 256
                || !row.value(QStringLiteral("enabled")).isBool())
                throw std::runtime_error("invalid_source_state");
            if (std::any_of(restored.begin(), restored.end(), [&source](const auto& existing) {
                    return existing.id == source.id
                        || (existing.module == source.module && existing.account == source.account
                            && existing.key == source.key);
                }))
                throw std::runtime_error("duplicate_source_state");
            restored.append(std::move(source));
        }
    }
    m_portable->database = database;
    m_portable->sources = std::move(restored);
    m_portable->restored = true;
    refreshSourceSnapshot();
}

QCoro::Task<void> ProviderRegistry::persistSources()
{
    QJsonArray records;
    for (const auto& source : m_portable->sources) {
        records.append(QJsonObject { { QStringLiteral("id"), source.id }, { QStringLiteral("module"), source.module },
            { QStringLiteral("account"), source.account }, { QStringLiteral("key"), source.key },
            { QStringLiteral("label"), source.label }, { QStringLiteral("enabled"), source.enabled } });
    }
    const QString value = QString::fromUtf8(
        QJsonDocument(QJsonObject { { QStringLiteral("format"), 1 }, { QStringLiteral("sources"), records } })
            .toJson(QJsonDocument::Compact));
    if (!m_portable->database)
        throw std::runtime_error("source_store_unavailable");
    QPointer<ProviderRegistry> guard(this);
    m_portable->database->saveSetting(stateKey, value);
    const QString stored = co_await m_portable->database->loadSettingAsync(stateKey);
    if (!guard)
        throw std::runtime_error("source_registry_removed");
    if (stored != value)
        throw std::runtime_error("source_state_write_failed");
}

QCoro::Task<QString> ProviderRegistry::configureSource(QString moduleId, QString accountId, QString sourceKey,
    QString label, QVariantMap configuration, QList<QUrl> authorisedOrigins)
{
    Q_ASSERT(thread() == QThread::currentThread());
    m_portable->requireReady();
    if (!m_portable->modules.contains(moduleId) || !boundedIdentity(accountId) || !boundedIdentity(sourceKey)
        || label.size() > 256)
        throw std::runtime_error("invalid_source_configuration");
    QPointer<ProviderRegistry> guard(this);
    m_portable->mutating = true;
    const auto unlock = qScopeGuard([guard] {
        if (guard)
            guard->m_portable->mutating = false;
    });
    auto found = std::find_if(m_portable->sources.begin(), m_portable->sources.end(), [&](const auto& source) {
        return source.module == moduleId && source.account == accountId && source.key == sourceKey;
    });
    const bool created = found == m_portable->sources.end();
    if (created) {
        if (m_portable->sources.size() >= 64)
            throw std::runtime_error("source_limit");
        PortableState::Source source;
        source.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        source.module = moduleId;
        source.account = accountId;
        source.key = sourceKey;
        m_portable->sources.append(std::move(source));
        found = std::prev(m_portable->sources.end());
    }
    const QString id = found->id;
    const QString oldLabel = found->label;
    found->label = std::move(label);
    try {
        co_await persistSources();
    } catch (...) {
        if (guard) {
            if (created)
                m_portable->sources.removeLast();
            else
                m_portable->find(id)->label = oldLabel;
        }
        throw;
    }
    if (!guard)
        throw std::runtime_error("source_registry_removed");
    auto *source = m_portable->find(id);
    ScriptRuntime *runtime = m_portable->modules.value(moduleId);
    const bool changed
        = !source->configured || source->configuration != configuration || source->origins != authorisedOrigins;
    if (changed) {
        if (source->active)
            runtime->removeSource(id);
        source->active = false;
        ++source->generation;
        source->configuration = std::move(configuration);
        source->origins = std::move(authorisedOrigins);
        source->configured = true;
    }
    if (source->enabled && !source->active) {
        const auto activate = runtime->addSource(id, source->configuration, source->origins);
        try {
            co_await activate;
        } catch (...) {
            if (guard)
                refreshSourceSnapshot();
            throw;
        }
        if (!guard)
            throw std::runtime_error("source_registry_removed");
        m_portable->find(id)->active = true;
    }
    refreshSourceSnapshot();
    co_return id;
}

QCoro::Task<QVariantMap> ProviderRegistry::callSource(
    QString sourceId, QString operation, QVariantMap arguments, QString scope)
{
    Q_ASSERT(thread() == QThread::currentThread());
    auto *source = m_portable->find(sourceId);
    if (!source || !source->enabled || !source->active)
        throw std::runtime_error("source_unavailable");
    QPointer<ProviderRegistry> guard(this);
    const quint64 generation = source->generation;
    const QString module = source->module;
    QVariantMap result;
    try {
        result = co_await m_portable->modules.value(module)->call(
            sourceId, std::move(operation), std::move(arguments), std::move(scope));
    } catch (const std::exception& error) {
        const QByteArray code(error.what());
        if (guard && (code == "script_interrupted" || code == "source_unavailable")) {
            for (auto& candidate : m_portable->sources) {
                if (candidate.module == module) {
                    candidate.active = false;
                    ++candidate.generation;
                }
            }
            refreshSourceSnapshot();
        }
        throw;
    }
    if (!guard)
        throw std::runtime_error("source_registry_removed");
    source = m_portable->find(sourceId);
    if (!source || !source->enabled || !source->active || source->generation != generation)
        throw std::runtime_error("source_generation_changed");
    co_return result;
}

QCoro::Task<ProviderMediaPage> ProviderRegistry::callSourceMediaPage(
    QString sourceId, QString operation, QVariantMap arguments, QString scope, int maximumItems)
{
    Q_ASSERT(thread() == QThread::currentThread());
    auto *source = m_portable->find(sourceId);
    if (!source || !source->enabled || !source->active)
        throw std::runtime_error("source_unavailable");
    QPointer<ProviderRegistry> guard(this);
    const quint64 generation = source->generation;
    const QString module = source->module;
    ProviderMediaPage result;
    try {
        result = co_await m_portable->modules.value(module)->callMediaPage(
            sourceId, std::move(operation), std::move(arguments), std::move(scope), maximumItems);
    } catch (const std::exception& error) {
        const QByteArray code(error.what());
        if (guard && (code == "script_interrupted" || code == "source_unavailable")) {
            for (auto& candidate : m_portable->sources) {
                if (candidate.module == module) {
                    candidate.active = false;
                    ++candidate.generation;
                }
            }
            refreshSourceSnapshot();
        }
        throw;
    }
    if (!guard)
        throw std::runtime_error("source_registry_removed");
    source = m_portable->find(sourceId);
    if (!source || !source->enabled || !source->active || source->generation != generation)
        throw std::runtime_error("source_generation_changed");
    co_return result;
}

void ProviderRegistry::cancelSourceScope(const QString& sourceId, const QString& scope)
{
    const auto *source = m_portable->find(sourceId);
    if (source && source->active) {
        if (auto *runtime = m_portable->modules.value(source->module))
            runtime->cancelScope(sourceId, scope);
    }
}

QCoro::Task<void> ProviderRegistry::setSourceEnabled(QString sourceId, bool enabled)
{
    Q_ASSERT(thread() == QThread::currentThread());
    m_portable->requireReady();
    auto *source = m_portable->find(sourceId);
    if (!source)
        throw std::runtime_error("source_unavailable");
    if (source->enabled == enabled)
        co_return;
    QPointer<ProviderRegistry> guard(this);
    m_portable->mutating = true;
    const auto unlock = qScopeGuard([guard] {
        if (guard)
            guard->m_portable->mutating = false;
    });
    ScriptRuntime *runtime = m_portable->modules.value(source->module);
    source->enabled = enabled;
    ++source->generation;
    if (source->active && runtime)
        runtime->removeSource(sourceId);
    source->active = false;
    refreshSourceSnapshot();
    co_await persistSources();
    if (!guard)
        throw std::runtime_error("source_registry_removed");
    source = m_portable->find(sourceId);
    if (enabled && source->configured && runtime) {
        co_await runtime->addSource(sourceId, source->configuration, source->origins);
        if (!guard)
            throw std::runtime_error("source_registry_removed");
        m_portable->find(sourceId)->active = true;
        refreshSourceSnapshot();
    }
}

QCoro::Task<void> ProviderRegistry::removeSource(QString sourceId)
{
    Q_ASSERT(thread() == QThread::currentThread());
    m_portable->requireReady();
    auto *source = m_portable->find(sourceId);
    if (!source)
        co_return;
    QPointer<ProviderRegistry> guard(this);
    m_portable->mutating = true;
    const auto unlock = qScopeGuard([guard] {
        if (guard)
            guard->m_portable->mutating = false;
    });
    if (auto *runtime = m_portable->modules.value(source->module))
        runtime->removeSource(sourceId);
    source->active = false;
    ++source->generation;
    const auto previous = m_portable->sources;
    m_portable->sources.removeIf([&sourceId](const auto& candidate) { return candidate.id == sourceId; });
    refreshSourceSnapshot();
    try {
        co_await persistSources();
    } catch (...) {
        if (guard) {
            m_portable->sources = previous;
            refreshSourceSnapshot();
        }
        throw;
    }
    if (!guard)
        throw std::runtime_error("source_registry_removed");
    emit sourceRemoved(sourceId);
}

QVariantList ProviderRegistry::configuredSources() const
{
    return m_portable->snapshot;
}

void ProviderRegistry::refreshSourceSnapshot()
{
    QVariantList snapshot;
    snapshot.reserve(m_portable->sources.size());
    for (const auto& source : m_portable->sources) {
        snapshot.append(QVariantMap { { QStringLiteral("sourceId"), source.id },
            { QStringLiteral("moduleId"), source.module }, { QStringLiteral("label"), source.label },
            { QStringLiteral("enabled"), source.enabled }, { QStringLiteral("available"), source.active } });
    }
    if (snapshot != m_portable->snapshot) {
        m_portable->snapshot = std::move(snapshot);
        emit configuredSourcesChanged();
    }
}

void ProviderRegistry::setNetworkAccessManager(QNetworkAccessManager *network)
{
    m_portable->network = network;
}

void ProviderRegistry::setProvidersDirectory(const QString& path)
{
    m_portable->providersDirectory = path;
}

QString ProviderRegistry::providersDirectory() const
{
    return m_portable->providersDirectory;
}

bool ProviderRegistry::hasModule(const QString& moduleId) const
{
    return m_portable->modules.contains(moduleId);
}

bool ProviderRegistry::isInstalled(const QString& moduleId) const
{
    if (moduleId == QStringLiteral("local"))
        return true;
    return hasModule(moduleId);
}

bool ProviderRegistry::isInstalling() const
{
    return m_portable->isInstalling;
}

QString ProviderRegistry::installStatus() const
{
    return m_portable->installStatus;
}

int ProviderRegistry::installProgress() const
{
    return m_portable->installProgress;
}

QString ProviderRegistry::installError() const
{
    return m_portable->installError;
}

bool ProviderRegistry::hasInstalledProviders() const
{
    return !m_portable->modules.isEmpty();
}

QVariantList ProviderRegistry::availableProviders() const
{
    QVariantList list;
    {
        QVariantMap jf;
        jf.insert(QStringLiteral("id"), QStringLiteral("spool.jellyfin"));
        jf.insert(QStringLiteral("name"), QStringLiteral("Jellyfin"));
        jf.insert(QStringLiteral("description"), QStringLiteral("Connect to your personal Jellyfin media server"));
        jf.insert(QStringLiteral("icon"), QStringLiteral("jellyfin"));
        jf.insert(QStringLiteral("installed"), hasModule(QStringLiteral("spool.jellyfin")));
        jf.insert(QStringLiteral("category"), QStringLiteral("Streaming Server"));
        jf.insert(QStringLiteral("requiresAuth"), true);
        list.append(jf);
    }
    {
        QVariantMap loc;
        loc.insert(QStringLiteral("id"), QStringLiteral("local"));
        loc.insert(QStringLiteral("name"), QStringLiteral("Local Videos"));
        loc.insert(QStringLiteral("description"), QStringLiteral("Browse videos stored on this device"));
        loc.insert(QStringLiteral("icon"), QStringLiteral("folder"));
        loc.insert(QStringLiteral("installed"), true);
        loc.insert(QStringLiteral("category"), QStringLiteral("Device"));
        loc.insert(QStringLiteral("requiresAuth"), false);
        list.append(loc);
    }
    return list;
}

void ProviderRegistry::scanInstalledModules()
{
    if (m_portable->providersDirectory.isEmpty())
        return;
    QDir dir(m_portable->providersDirectory);
    if (!dir.exists())
        return;
    const auto entries = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& info : entries) {
        const QDir subDir(info.absoluteFilePath());
        const QString manifestPath = subDir.filePath(QStringLiteral("manifest.json"));
        if (!QFile::exists(manifestPath))
            continue;
        QFile manifestFile(manifestPath);
        if (!manifestFile.open(QIODevice::ReadOnly))
            continue;
        const auto doc = QJsonDocument::fromJson(manifestFile.readAll());
        if (!doc.isObject())
            continue;
        const auto obj = doc.object();
        const QString id = obj.value(QStringLiteral("id")).toString();
        const QString entry = obj.value(QStringLiteral("entry")).toString();
        if (!id.isEmpty() && !entry.isEmpty() && !m_portable->modules.contains(id)) {
            const QString entryPoint = subDir.filePath(entry);
            if (QFile::exists(entryPoint)) {
                try {
                    registerModule(id, entryPoint);
                } catch (const std::exception& e) {
                    qWarning("Failed to register installed module %s: %s", qPrintable(id), e.what());
                }
            }
        }
    }
    emit installedProvidersChanged();
    emit availableProvidersChanged();
}

void ProviderRegistry::installProvider(const QString& moduleId)
{
    if (m_portable->isInstalling)
        return;
    downloadAndInstallProvider(moduleId);
}

bool ProviderRegistry::switchProvider(const QString& providerId)
{
    return setActive(providerId);
}

QCoro::Task<bool> ProviderRegistry::downloadAndInstallProvider(QString moduleId, QUrl url)
{
    if (m_portable->isInstalling)
        co_return false;

    m_portable->isInstalling = true;
    m_portable->installProgress = 5;
    m_portable->installStatus = QStringLiteral("Connecting...");
    m_portable->installError.clear();
    emit installStatusChanged();

    if (url.isEmpty()) {
        if (moduleId == QStringLiteral("spool.jellyfin")) {
            url = QUrl(QStringLiteral("https://github.com/spool-player/spool-jellyfin/releases/download/v0.1.0-alpha.1/"
                                      "spool-jellyfin-0.1.0-alpha.1.zip"));
        } else {
            m_portable->isInstalling = false;
            m_portable->installError = QStringLiteral("Unknown provider module: %1").arg(moduleId);
            emit installStatusChanged();
            co_return false;
        }
    }

    QByteArray archiveData;
    bool downloadSuccess = false;

    if (m_portable->network) {
        QUrl currentUrl = url;
        int redirectCount = 0;
        constexpr int maxRedirects = 5;

        while (redirectCount < maxRedirects) {
            QNetworkRequest request(currentUrl);
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
            request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Spool/0.8.0"));

            QNetworkReply *reply = m_portable->network->get(request);
            connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
                if (total > 0) {
                    m_portable->installProgress = std::clamp(static_cast<int>(10 + (received * 75) / total), 10, 85);
                    m_portable->installStatus = QStringLiteral("Downloading (%1%)...").arg(m_portable->installProgress);
                    emit installStatusChanged();
                }
            });

            co_await reply;

            const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (statusCode == 301 || statusCode == 302 || statusCode == 303 || statusCode == 307 || statusCode == 308) {
                const QVariant redirectVar = reply->header(QNetworkRequest::LocationHeader);
                if (redirectVar.isValid()) {
                    currentUrl = currentUrl.resolved(redirectVar.toUrl());
                    reply->deleteLater();
                    ++redirectCount;
                    continue;
                }
            }

            if (reply->error() == QNetworkReply::NoError && statusCode >= 200 && statusCode < 300) {
                archiveData = reply->readAll();
                downloadSuccess = true;
            } else {
                qWarning("Download from %s failed with status %d: %s", qPrintable(currentUrl.toString()), statusCode,
                    qPrintable(reply->errorString()));
            }
            reply->deleteLater();
            break;
        }
    }

    // If online download failed or no network, check local fallback files
    if (!downloadSuccess || archiveData.isEmpty()) {
        const QString fallbackPath = QStringLiteral("providers/bundled/spool-jellyfin-0.1.0-alpha.1.zip");
        if (QFile::exists(fallbackPath)) {
            QFile file(fallbackPath);
            if (file.open(QIODevice::ReadOnly)) {
                archiveData = file.readAll();
                downloadSuccess = true;
            }
        }
    }

    if (!downloadSuccess || archiveData.isEmpty()) {
        m_portable->isInstalling = false;
        m_portable->installError = QStringLiteral("Failed to download provider package.");
        emit installStatusChanged();
        co_return false;
    }

    m_portable->installStatus = QStringLiteral("Validating package...");
    m_portable->installProgress = 90;
    emit installStatusChanged();

    QString errorMsg;
    auto parsed = ProviderPackage::parseAndValidate(archiveData, &errorMsg);
    if (!parsed) {
        m_portable->isInstalling = false;
        m_portable->installError = QStringLiteral("Package validation failed: %1").arg(errorMsg);
        emit installStatusChanged();
        co_return false;
    }

    m_portable->installStatus = QStringLiteral("Installing files...");
    m_portable->installProgress = 95;
    emit installStatusChanged();

    const QString entryPoint = ProviderPackage::install(*parsed, m_portable->providersDirectory, &errorMsg);
    if (entryPoint.isEmpty()) {
        m_portable->isInstalling = false;
        m_portable->installError = QStringLiteral("Installation failed: %1").arg(errorMsg);
        emit installStatusChanged();
        co_return false;
    }

    try {
        if (!m_portable->modules.contains(parsed->id))
            registerModule(parsed->id, entryPoint);
    } catch (const std::exception& e) {
        m_portable->isInstalling = false;
        m_portable->installError = QString::fromUtf8(e.what());
        emit installStatusChanged();
        co_return false;
    }

    if (parsed->id == QStringLiteral("spool.jellyfin")) {
        setActive(QStringLiteral("jellyfin"));
    }

    m_portable->isInstalling = false;
    m_portable->installProgress = 100;
    m_portable->installStatus = QStringLiteral("Ready");
    emit installStatusChanged();
    emit installedProvidersChanged();
    emit availableProvidersChanged();
    emit providerInstalled(moduleId);

    co_return true;
}

} // namespace JellyfinNative
