#include "ProviderStore.h"

#include "../cache/DatabaseManager.h"
#include "../common/AsyncTask.h"
#include "ProviderPackage.h"
#include "ProviderRegistry.h"

#include <QCoroFuture>
#include <QCoroNetworkReply>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>

namespace JellyfinNative {

namespace {
    const QString kOriginsKey = QStringLiteral("providers/origins/1");
    constexpr qint64 kCatalogLimit = 2 * 1024 * 1024;
    constexpr qint64 kPackageLimit = 16 * 1024 * 1024;

    QString text(const QVariantMap& map, const char *key)
    {
        return map.value(QLatin1String(key)).toString();
    }

    // A catalogue or feed entry is only usable with every field an install
    // needs; anything else is skipped rather than half-shown.
    bool validEntry(const QVariantMap& entry)
    {
        static const QRegularExpression sha(QStringLiteral("^[0-9a-f]{64}$"));
        const QUrl url(text(entry, "url"));
        return !text(entry, "id").isEmpty() && !text(entry, "name").isEmpty() && !text(entry, "version").isEmpty()
            && url.scheme() == QStringLiteral("https") && sha.match(text(entry, "sha256")).hasMatch();
    }
} // namespace

ProviderStore::ProviderStore(ProviderRegistry *registry, DatabaseManager *database, QNetworkAccessManager *network,
    QUrl catalogBase, QObject *parent)
    : QObject(parent)
    , m_registry(registry)
    , m_database(database)
    , m_network(network)
    , m_catalogBase(std::move(catalogBase))
{
    connect(registry, &ProviderRegistry::modulesChanged, this, &ProviderStore::catalogChanged);
}

QUrl ProviderStore::feedUrlFor(const QString& input)
{
    QUrl url = QUrl::fromUserInput(input.trimmed());
    if (!url.isValid() || url.host().isEmpty())
        return {};
    url.setScheme(QStringLiteral("https"));
    QString path = url.path();
    if (path.endsWith(QStringLiteral(".json")) || path.endsWith(QStringLiteral(".tar.zst")))
        return url;
    if (path.endsWith(QStringLiteral(".git")))
        path.chop(4);
    const QString host = url.host().toLower();
    QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    // A project page: releases publish spool-provider.json as an asset, and
    // both forges keep a stable link to the latest release's copy.
    if (host == QStringLiteral("github.com") && parts.size() >= 2)
        return QUrl(QStringLiteral("https://github.com/%1/%2/releases/latest/download/spool-provider.json")
                .arg(parts.at(0), parts.at(1)));
    if (host == QStringLiteral("gitlab.com") || host.startsWith(QStringLiteral("gitlab."))) {
        const qsizetype marker = parts.indexOf(QStringLiteral("-"));
        if (marker >= 0)
            parts = parts.mid(0, marker);
        if (parts.size() >= 2)
            return QUrl(QStringLiteral("https://%1/%2/-/releases/permalink/latest/downloads/spool-provider.json")
                    .arg(host, parts.join(QLatin1Char('/'))));
    }
    // Anything else is a static site (GitHub or GitLab Pages, a CDN) that
    // serves the file at its root.
    url.setPath(
        (path.endsWith(QLatin1Char('/')) ? path : path + QLatin1Char('/')) + QStringLiteral("spool-provider.json"));
    return url;
}

QCoro::Task<QByteArray> ProviderStore::fetch(QUrl url, qint64 limit)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Spool"));
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(20000);
    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply, limit](qint64 received, qint64) {
        if (received > limit)
            reply->abort();
    });
    co_await reply;
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300)
        throw std::runtime_error(status == 404 ? "not_found" : "download_failed");
    co_return reply->readAll();
}

QCoro::Task<QVariantList> ProviderStore::fetchCatalog(QString name)
{
    const QByteArray data = co_await fetch(m_catalogBase.resolved(QUrl(name)), kCatalogLimit);
    const QJsonObject root = QJsonDocument::fromJson(data).object();
    QVariantList entries;
    for (const QJsonValue& value : root.value(QStringLiteral("providers")).toArray()) {
        QVariantMap entry = value.toObject().toVariantMap();
        if (!validEntry(entry))
            continue;
        if (!text(entry, "icon").isEmpty())
            entry.insert(QStringLiteral("iconUrl"), m_catalogBase.resolved(QUrl(text(entry, "icon"))));
        entries.append(entry);
    }
    co_return entries;
}

void ProviderStore::refresh(bool includeCommunity)
{
    const auto load = [this](QString name, bool official) {
        ++m_loading;
        emit catalogChanged();
        Async::runScoped(
            this, fetchCatalog(name),
            [this, official](QVariantList entries) {
                --m_loading;
                m_error.clear();
                if (official)
                    m_official = std::move(entries);
                else
                    m_community = std::move(entries);
                emit catalogChanged();
            },
            [this](const std::exception_ptr&) {
                --m_loading;
                m_error = QStringLiteral("Can't reach the provider store right now");
                emit catalogChanged();
            },
            "provider catalogue");
    };
    load(QStringLiteral("official.json"), true);
    if (includeCommunity)
        load(QStringLiteral("index.json"), false);
}

QVariantList ProviderStore::annotate(const QVariantList& entries) const
{
    QVariantList result;
    for (const QVariant& value : entries) {
        QVariantMap entry = value.toMap();
        const QString id = text(entry, "id");
        const ProviderModule *module = m_registry ? m_registry->module(id) : nullptr;
        entry.insert(QStringLiteral("installed"), module != nullptr);
        entry.insert(QStringLiteral("installedVersion"), module ? module->manifest.version : QString());
        entry.insert(QStringLiteral("compatible"), text(entry, "api") == QLatin1String(kApi));
        entry.insert(QStringLiteral("updateAvailable"),
            module && ProviderPackage::compareVersions(text(entry, "version"), module->manifest.version) > 0);
        entry.insert(QStringLiteral("busy"), m_busy.value(id).toString());
        if (module && !entry.contains(QStringLiteral("iconUrl")))
            entry.insert(QStringLiteral("iconUrl"), module->file(module->manifest.icon));
        result.append(entry);
    }
    return result;
}

QVariantList ProviderStore::official() const
{
    QVariantList entries = m_official;
    QStringList listed;
    for (const QVariant& entry : std::as_const(entries))
        listed.append(entry.toMap().value(QStringLiteral("id")).toString());
    // Bundled providers are official too, and must show offline.
    if (m_registry) {
        for (const QVariant& value : m_registry->modules()) {
            const QVariantMap module = value.toMap();
            const QString id = text(module, "id");
            if (listed.contains(id) || m_origins.value(id).channel == QStringLiteral("community")
                || m_origins.value(id).channel == QStringLiteral("url"))
                continue;
            if (module.value(QStringLiteral("bundled")).toBool()
                || m_origins.value(id).channel == QStringLiteral("official")) {
                QVariantMap entry = module;
                entry.insert(QStringLiteral("api"), QLatin1String(kApi));
                entries.append(entry);
            }
        }
    }
    return annotate(entries);
}

QVariantList ProviderStore::community() const
{
    QVariantList entries;
    QStringList listed;
    for (const QVariant& value : m_community) {
        if (!value.toMap().value(QStringLiteral("official")).toBool()) {
            entries.append(value);
            listed.append(text(value.toMap(), "id"));
        }
    }
    // Providers added by URL sit with the community ones, marked as such.
    if (m_registry) {
        for (auto it = m_origins.cbegin(); it != m_origins.cend(); ++it) {
            const ProviderModule *module = m_registry->module(it.key());
            if (it->channel != QStringLiteral("url") || !module || listed.contains(it.key()))
                continue;
            entries.append(QVariantMap { { QStringLiteral("id"), it.key() },
                { QStringLiteral("name"), module->manifest.name },
                { QStringLiteral("summary"), module->manifest.summary },
                { QStringLiteral("version"), module->manifest.version }, { QStringLiteral("api"), QLatin1String(kApi) },
                { QStringLiteral("publisher"), it->feed.host() }, { QStringLiteral("fromUrl"), true } });
        }
    }
    return annotate(entries);
}

QVariantMap ProviderStore::entryFor(const QString& id) const
{
    for (const QVariantList *list : { &m_official, &m_community, &m_updates }) {
        for (const QVariant& value : *list) {
            if (text(value.toMap(), "id") == id && validEntry(value.toMap()))
                return value.toMap();
        }
    }
    return {};
}

void ProviderStore::setBusy(const QString& id, const QString& state)
{
    if (state.isEmpty())
        m_busy.remove(id);
    else
        m_busy.insert(id, state);
    emit busyChanged();
    emit catalogChanged();
}

void ProviderStore::install(const QString& id)
{
    const QVariantMap entry = entryFor(id);
    if (entry.isEmpty() || m_busy.contains(id))
        return;
    const bool official = std::any_of(
        m_official.begin(), m_official.end(), [&id](const QVariant& value) { return text(value.toMap(), "id") == id; });
    Async::runScoped(
        this, installEntry(entry, { official ? QStringLiteral("official") : QStringLiteral("community"), {} }), [] { },
        [](const std::exception_ptr&) { }, "provider install");
}

void ProviderStore::update(const QString& id)
{
    const QVariantMap entry = entryFor(id);
    if (entry.isEmpty() || m_busy.contains(id))
        return;
    const Origin origin = m_origins.value(id, { QStringLiteral("official"), {} });
    Async::runScoped(this, installEntry(entry, origin), [] { }, [](const std::exception_ptr&) { }, "provider update");
}

void ProviderStore::updateAll()
{
    for (const QVariant& value : QVariantList(m_updates))
        update(text(value.toMap(), "id"));
}

void ProviderStore::uninstall(const QString& id)
{
    m_origins.remove(id);
    saveOrigins();
    Async::runScoped(this, m_registry->uninstall(id), [] { }, [](const std::exception_ptr&) { }, "provider remove");
}

void ProviderStore::addFromUrl(const QString& input)
{
    const QUrl feed = feedUrlFor(input);
    if (feed.isEmpty()) {
        emit problem(QStringLiteral("That doesn't look like a link"));
        return;
    }
    const auto add = [](ProviderStore *self, QUrl feed) -> QCoro::Task<void> {
        self->setBusy(QStringLiteral("url"), QStringLiteral("downloading"));
        try {
            const QByteArray data = co_await self->fetch(feed, kCatalogLimit);
            const QVariantMap entry = QJsonDocument::fromJson(data).object().toVariantMap();
            if (!validEntry(entry))
                throw std::runtime_error("invalid_feed");
            self->setBusy(QStringLiteral("url"), {});
            co_await self->installEntry(entry, { QStringLiteral("url"), feed });
        } catch (const std::exception& error) {
            self->setBusy(QStringLiteral("url"), {});
            emit self->problem(QByteArray(error.what()) == "not_found"
                    ? QStringLiteral("No spool-provider.json was found there")
                    : QStringLiteral("That link isn't a Spool provider"));
        }
    };
    Async::runScoped(this, add(this, feed), [] { }, [](const std::exception_ptr&) { }, "provider add");
}

QCoro::Task<void> ProviderStore::installEntry(QVariantMap entry, Origin origin)
{
    const QString id = text(entry, "id");
    const QString name = text(entry, "name");
    if (text(entry, "api") != QLatin1String(kApi)) {
        emit problem(QStringLiteral("%1 needs a different version of Spool").arg(name));
        co_return;
    }
    QPointer<ProviderStore> guard(this);
    setBusy(id, QStringLiteral("downloading"));
    try {
        const QByteArray archive = co_await fetch(QUrl(text(entry, "url")), kPackageLimit);
        if (!guard)
            co_return;
        setBusy(id, QStringLiteral("installing"));
        const QString expected = text(entry, "sha256");
        // Hashing and unpacking stay off the GUI thread.
        const auto package
            = co_await Async::background([archive, expected]() -> std::optional<ProviderPackageContents> {
                  if (QCryptographicHash::hash(archive, QCryptographicHash::Sha256).toHex() != expected.toLatin1())
                      return std::nullopt;
                  return ProviderPackage::read(archive);
              });
        if (!guard)
            co_return;
        if (!package || package->manifest.id != id || package->manifest.version != text(entry, "version"))
            throw std::runtime_error("package_mismatch");
        co_await m_registry->install(*package);
        if (!guard)
            co_return;
        m_origins.insert(id, origin);
        saveOrigins();
        m_updates.erase(std::remove_if(m_updates.begin(), m_updates.end(),
                            [&id](const QVariant& value) { return text(value.toMap(), "id") == id; }),
            m_updates.end());
        emit updatesChanged();
        setBusy(id, {});
        emit installed(id, name);
    } catch (const std::exception& error) {
        if (!guard)
            co_return;
        setBusy(id, {});
        qWarning("providers: installing %s failed: %s", qPrintable(id), error.what());
        emit problem(QByteArray(error.what()) == "package_mismatch"
                ? QStringLiteral("%1 didn't match what was published, so it wasn't installed").arg(name)
                : QStringLiteral("Couldn't download %1").arg(name));
    }
}

void ProviderStore::checkForUpdates(const QString& policy)
{
    Async::runScoped(this, checkAsync(policy), [] { }, [](const std::exception_ptr&) { }, "provider update check");
}

QCoro::Task<void> ProviderStore::checkAsync(QString policy)
{
    QPointer<ProviderStore> guard(this);
    if (!m_originsLoaded) {
        const QString stored = co_await m_database->loadSettingAsync(kOriginsKey);
        if (!guard)
            co_return;
        const QJsonObject root = QJsonDocument::fromJson(stored.toUtf8()).object();
        for (auto it = root.begin(); it != root.end(); ++it) {
            const QJsonObject origin = it.value().toObject();
            m_origins.insert(it.key(),
                { origin.value(QStringLiteral("channel")).toString(),
                    QUrl(origin.value(QStringLiteral("feed")).toString()) });
        }
        m_originsLoaded = true;
    }
    const bool wantsCommunity = std::any_of(m_origins.begin(), m_origins.end(),
        [](const Origin& origin) { return origin.channel == QStringLiteral("community"); });
    // One small file for first-party providers; the full index only when
    // something from it is installed.
    try {
        m_official = co_await fetchCatalog(QStringLiteral("official.json"));
        if (wantsCommunity && guard)
            m_community = co_await fetchCatalog(QStringLiteral("index.json"));
    } catch (const std::exception&) {
        co_return; // Offline: nothing to offer, nothing to report.
    }
    if (!guard)
        co_return;
    QVariantList updates;
    for (const QString& id : m_registry->moduleIds()) {
        const ProviderModule *module = m_registry->module(id);
        const Origin origin = m_origins.value(id, { QStringLiteral("official"), {} });
        QVariantMap latest;
        if (origin.channel == QStringLiteral("url")) {
            try {
                latest = QJsonDocument::fromJson(co_await fetch(origin.feed, kCatalogLimit)).object().toVariantMap();
            } catch (const std::exception&) {
                continue;
            }
            if (!guard)
                co_return;
        } else {
            for (const QVariant& value : origin.channel == QStringLiteral("community") ? m_community : m_official) {
                if (text(value.toMap(), "id") == id)
                    latest = value.toMap();
            }
        }
        if (module && validEntry(latest) && text(latest, "api") == QLatin1String(kApi)
            && ProviderPackage::compareVersions(text(latest, "version"), module->manifest.version) > 0)
            updates.append(latest);
    }
    m_updates = updates;
    emit updatesChanged();
    emit catalogChanged();
    if (policy == QStringLiteral("auto"))
        updateAll();
}

void ProviderStore::saveOrigins()
{
    QJsonObject root;
    for (auto it = m_origins.cbegin(); it != m_origins.cend(); ++it)
        root.insert(it.key(),
            QJsonObject {
                { QStringLiteral("channel"), it->channel }, { QStringLiteral("feed"), it->feed.toString() } });
    m_database->saveSetting(kOriginsKey, QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

} // namespace JellyfinNative
