#include "ProviderStore.h"

#include "../cache/DatabaseManager.h"
#include "../common/AsyncTask.h"
#include "ProviderPackage.h"
#include "ProviderRegistry.h"

#include <QCoreApplication>
#include <QCoroFuture>
#include <QCoroNetworkReply>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Spool {

namespace {
    const QString kOriginsKey = QStringLiteral("providers/origins/1");
    constexpr qint64 kCatalogLimit = 2 * 1024 * 1024;
    constexpr qint64 kPackageLimit = 16 * 1024 * 1024;

    QString text(const QVariantMap& map, const char *key)
    {
        return map.value(QLatin1String(key)).toString();
    }

    // Called only in a worker. On Unix, nonblocking open plus fstat also
    // closes the regular-file -> FIFO substitution race.
    bool openRegularFile(QFile& file, const QUrl& url)
    {
        if (!url.isLocalFile() || !QFileInfo(url.toLocalFile()).isFile())
            return false;
        file.setFileName(url.toLocalFile());
#ifdef Q_OS_UNIX
        const int fd = ::open(QFile::encodeName(url.toLocalFile()).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            return false;
        struct stat info {};
        if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)
            || !file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
            ::close(fd);
            return false;
        }
        return true;
#else
        return file.open(QIODevice::ReadOnly);
#endif
    }

    bool packageMagic(const QByteArray& magic)
    {
        return magic == QByteArrayLiteral("\x28\xb5\x2f\xfd")
            || (magic.size() == 4 && (static_cast<unsigned char>(magic[0]) & 0xf0) == 0x50
                && magic.mid(1) == QByteArrayLiteral("\x2a\x4d\x18"));
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

std::optional<ProviderSources> providerSourcesFromName(QStringView name)
{
    if (name == QLatin1String("bundled"))
        return ProviderSources::Bundled;
    if (name == QLatin1String("curated"))
        return ProviderSources::Curated;
    if (name == QLatin1String("open"))
        return ProviderSources::Open;
    return std::nullopt;
}

ProviderStore::ProviderStore(ProviderRegistry *registry, DatabaseManager *database, QNetworkAccessManager *network,
    QUrl catalogBase, ProviderSources sources, QObject *parent)
    : QObject(parent)
    , m_registry(registry)
    , m_database(database)
    , m_network(network)
    , m_catalogBase(std::move(catalogBase))
    , m_sources(sources)
{
    connect(registry, &ProviderRegistry::modulesChanged, this, [this] {
        ++m_registryRevision;
        cancelInspection();
        emit catalogChanged();
    });
    if (storeAvailable())
        Async::runScoped(this, loadOrigins(), [] { }, [](const std::exception_ptr&) { }, "provider origins");
}

QCoro::Task<void> ProviderStore::loadOrigins()
{
    if (m_originsLoaded)
        co_return;
    QPointer<ProviderStore> guard(this);
    const QString stored = co_await m_database->loadSettingAsync(kOriginsKey);
    // Two callers may race here; the first one to finish wins.
    if (!guard || m_originsLoaded)
        co_return;
    const QJsonObject root = QJsonDocument::fromJson(stored.toUtf8()).object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        const QJsonObject origin = it.value().toObject();
        m_origins.insert(it.key(),
            { origin.value(QStringLiteral("channel")).toString(),
                QUrl(origin.value(QStringLiteral("feed")).toString()) });
    }
    m_originsLoaded = true;
    emit catalogChanged();
}

QUrl ProviderStore::feedUrlFor(const QString& input)
{
    QUrl url = QUrl::fromUserInput(input.trimmed());
    if (!url.isValid() || url.host().isEmpty())
        return {};
    url.setScheme(QStringLiteral("https"));
    QString path = url.path();
    if (path.endsWith(QStringLiteral(".json")))
        return url;
    // A package link from a release: its feed is published next to it.
    if (path.endsWith(QStringLiteral(".szo")) || path.endsWith(QStringLiteral(".tar.zst"))) {
        url.setPath(path.left(path.lastIndexOf(QLatin1Char('/')) + 1) + QStringLiteral("spool-provider.json"));
        return url;
    }
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

QCoro::Task<QByteArray> ProviderStore::fetch(QUrl url, qint64 limit, QString transferId)
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
    if (!transferId.isEmpty()) {
        connect(reply, &QNetworkReply::downloadProgress, this, [this, transferId](qint64 received, qint64 total) {
            auto progress = m_transferProgress.value(transferId).toMap();
            progress.insert(QStringLiteral("received"), received);
            progress.insert(QStringLiteral("total"), total);
            m_transferProgress.insert(transferId, progress);
            emit busyChanged();
        });
    }
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
    if (!storeAvailable())
        return;
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
        entry.insert(QStringLiteral("compatible"), entry.value(QStringLiteral("format")).toInt() == 3);
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
                entry.insert(QStringLiteral("format"), 3);
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
    // Everything installed is listed, catalogue or not: added by link,
    // installed while the store was reachable, or copied in by hand.
    if (m_registry) {
        QStringList shown = listed;
        for (const QVariant& value : official())
            shown.append(text(value.toMap(), "id"));
        for (const QString& id : m_registry->moduleIds()) {
            const ProviderModule *module = m_registry->module(id);
            if (!module || shown.contains(id))
                continue;
            const Origin origin = m_origins.value(id);
            entries.append(
                QVariantMap { { QStringLiteral("id"), id }, { QStringLiteral("name"), module->manifest.name },
                    { QStringLiteral("summary"), module->manifest.summary },
                    { QStringLiteral("version"), module->manifest.version }, { QStringLiteral("format"), 3 },
                    { QStringLiteral("publisher"),
                        origin.channel == QStringLiteral("url") ? origin.feed.host() : module->manifest.publisher },
                    { QStringLiteral("fromUrl"), origin.channel == QStringLiteral("url") } });
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

QVariantList ProviderStore::transfers() const
{
    QVariantList result;
    for (auto it = m_transferProgress.cbegin(); it != m_transferProgress.cend(); ++it) {
        auto row = it.value().toMap();
        row.insert(QStringLiteral("id"), it.key());
        row.insert(QStringLiteral("state"), m_busy.value(it.key()));
        result.append(row);
    }
    return result;
}

void ProviderStore::setBusy(const QString& id, const QString& state)
{
    if (state.isEmpty()) {
        m_busy.remove(id);
        m_transferProgress.remove(id);
    } else
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
    if (!storeAvailable())
        return;
    const auto remove = [](ProviderStore *self, QString id) -> QCoro::Task<void> {
        QPointer<ProviderStore> guard(self);
        co_await self->loadOrigins();
        if (!guard)
            co_return;
        self->m_origins.remove(id);
        self->saveOrigins();
        co_await self->m_registry->uninstall(id);
    };
    Async::runScoped(this, remove(this, id), [] { }, [](const std::exception_ptr&) { }, "provider remove");
}

void ProviderStore::addFromUrl(const QString& input)
{
    if (!linksAllowed()) {
        emit problem(QStringLiteral("This version of Spool only installs providers from its store"));
        return;
    }
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

QVariantMap ProviderStore::inspectedPackage() const
{
    return m_inspection ? m_inspection->preview : QVariantMap {};
}

QString ProviderStore::provenance(const QString& id) const
{
    const ProviderModule *module = m_registry ? m_registry->module(id) : nullptr;
    if (!module)
        return {};
    if (module->native)
        return QStringLiteral("native");
    if (module->bundled && !module->overridesBundled)
        return QStringLiteral("bundled");
    const QString channel = m_origins.value(id).channel;
    if (channel == QStringLiteral("official") || channel == QStringLiteral("community")
        || channel == QStringLiteral("url") || channel == QStringLiteral("file"))
        return channel;
    return QStringLiteral("unknown");
}

QVariantList ProviderStore::installedProviders() const
{
    QVariantList result;
    if (m_registry) {
        for (const QString& id : m_registry->moduleIds())
            result.append(
                QVariantMap { { QStringLiteral("id"), id }, { QStringLiteral("provenance"), provenance(id) } });
    }
    return result;
}

bool ProviderStore::permitsFileReplacement(const QString& id) const
{
    if (!m_registry)
        return false;
    const ProviderModule *module = m_registry->module(id);
    return !module
        || (!module->native && !module->bundled && !module->overridesBundled
            && m_origins.value(id).channel == QStringLiteral("file"));
}

bool ProviderStore::isPackageCandidate(const QUrl& url) const
{
    // A cheap naming hint only. Content classification happens in a worker.
    return url.isLocalFile() && url.fileName().endsWith(QStringLiteral(".szo"), Qt::CaseInsensitive);
}

QString ProviderStore::classifyFiles(const QVariantList& urls)
{
    const QString requestId = QUuid::createUuid().toString(QUuid::Id128);
    if (m_classifying || urls.isEmpty() || urls.size() > 256) {
        QTimer::singleShot(0, this, [this, requestId] {
            emit filesClassified(
                requestId, {}, {}, QStringLiteral("Choose up to 256 files after the current drop finishes."));
        });
        return requestId;
    }
    m_classifying = true;
    QTimer::singleShot(0, this, [this, requestId, urls] {
        struct Result {
            QVariantList packages;
            QVariantList media;
            QString error;
        };
        const auto work = [urls] {
            Result result;
            for (const QVariant& value : urls) {
                const QUrl url = value.toUrl();
                if (!url.isValid() || url.isEmpty()) {
                    result.error = QStringLiteral("The drop contains an invalid file location");
                    break;
                }
                if (!url.isLocalFile()) {
                    result.media.append(url);
                    continue;
                }
                QFile file;
                if (!openRegularFile(file, url)) {
                    result.error = QStringLiteral("The drop contains an unreadable or special file");
                    break;
                }
                const QByteArray magic = file.read(4);
                if (file.error() != QFileDevice::NoError) {
                    result.error = QStringLiteral("Couldn't inspect a dropped file");
                    break;
                }
                if (url.fileName().endsWith(QStringLiteral(".szo"), Qt::CaseInsensitive) || packageMagic(magic))
                    result.packages.append(url);
                else
                    result.media.append(url);
            }
            return result;
        };
        const auto classify
            = [](decltype(work) work) -> QCoro::Task<Result> { co_return co_await Async::background(std::move(work)); };
        Async::runScoped(
            this, classify(work),
            [this, requestId](Result result) {
                m_classifying = false;
                emit filesClassified(requestId, result.error.isEmpty() ? result.packages : QVariantList {},
                    result.error.isEmpty() ? result.media : QVariantList {}, result.error);
            },
            [this, requestId](const std::exception_ptr&) {
                m_classifying = false;
                emit filesClassified(requestId, {}, {}, QStringLiteral("Couldn't classify the dropped files"));
            },
            "provider drop classification");
    });
    return requestId;
}

void ProviderStore::cancelInspection(const QString& operationId)
{
    if (!operationId.isEmpty() && operationId != m_inspectionRequestId && operationId != m_installingToken
        && (!m_inspection || operationId != m_inspection->token))
        return;
    ++m_inspectionGeneration;
    if (m_inspection) {
        m_inspection.reset();
        emit inspectedPackageChanged();
    }
}

void ProviderStore::failInspection(const QString& requestId, const QString& error)
{
    emit fileInspectionFinished(requestId, {}, error);
    emit problem(error);
}

QString ProviderStore::inspectFile(const QUrl& url)
{
    const QString requestId = QUuid::createUuid().toString(QUuid::Id128);
    QString error;
    if (!linksAllowed())
        error = QStringLiteral("This version of Spool does not install providers from local files");
    else if (m_inspecting || m_fileInstalling)
        error = QStringLiteral("A provider file is already being processed. Please wait.");
    else if (!url.isLocalFile())
        error = QStringLiteral("Choose a local provider package file");
    if (!error.isEmpty()) {
        QTimer::singleShot(0, this, [this, requestId, error] { failInspection(requestId, error); });
        return requestId;
    }
    cancelInspection();
    m_inspectionRequestId = requestId;
    m_inspecting = true;
    const quint64 generation = m_inspectionGeneration;
    setBusy(QStringLiteral("file"), QStringLiteral("inspecting"));
    QTimer::singleShot(0, this, [this, url, generation, requestId] {
        Async::runScoped(
            this, inspectFileAsync(url, generation, requestId), [] {},
            [this, requestId](const std::exception_ptr&) {
                m_inspecting = false;
                setBusy(QStringLiteral("file"), {});
                failInspection(requestId, QStringLiteral("Couldn't inspect that provider package"));
            },
            "provider file inspection");
    });
    return requestId;
}

QCoro::Task<void> ProviderStore::inspectFileAsync(QUrl url, quint64 generation, QString requestId)
{
    QPointer<ProviderStore> guard(this);
    co_await loadOrigins();
    if (!guard)
        co_return;
    if (generation != m_inspectionGeneration) {
        m_inspecting = false;
        setBusy(QStringLiteral("file"), {});
        emit fileInspectionFinished(requestId, {}, QStringLiteral("Provider inspection cancelled"));
        co_return;
    }
    const quint64 revision = m_registryRevision;
    struct Result {
        std::optional<ProviderPackageContents> package;
        QString digest;
        QString error;
    };
    auto result = co_await Async::background([url = std::move(url)] {
        Result result;
        QFile file;
        if (!openRegularFile(file, url)) {
            result.error = QStringLiteral("Couldn't open that provider package");
            return result;
        }
        if (file.size() <= 0 || file.size() > kPackageLimit) {
            result.error = QStringLiteral("Provider package is empty or exceeds 16 MiB");
            return result;
        }
        const QByteArray archive = file.read(kPackageLimit + 1);
        if (file.error() != QFileDevice::NoError || !file.atEnd() || archive.size() > kPackageLimit) {
            result.error = QStringLiteral("Couldn't read a bounded provider package");
            return result;
        }
        result.package = ProviderPackage::read(archive, &result.error);
        if (result.package)
            result.digest = QString::fromLatin1(QCryptographicHash::hash(archive, QCryptographicHash::Sha256).toHex());
        return result;
    });
    if (!guard)
        co_return;
    m_inspecting = false;
    setBusy(QStringLiteral("file"), {});
    if (generation != m_inspectionGeneration) {
        emit fileInspectionFinished(requestId, {}, QStringLiteral("Provider inspection cancelled"));
        co_return;
    }
    if (revision != m_registryRevision) {
        failInspection(
            requestId, QStringLiteral("Installed providers changed. Inspect the file again before approving it."));
        co_return;
    }
    if (!result.package) {
        // Decoder details may contain attacker-controlled paths; keep UI errors bounded.
        failInspection(requestId, QStringLiteral("Invalid provider package: %1").arg(result.error.left(240)));
        co_return;
    }
    const auto& manifest = result.package->manifest;
    if (!permitsFileReplacement(manifest.id)) {
        failInspection(requestId,
            QStringLiteral("An unverified local file cannot replace this installed provider. "
                           "Use its existing store or URL update source."));
        co_return;
    }
    if (m_busy.contains(manifest.id)) {
        failInspection(requestId,
            QStringLiteral("That provider is already being changed. Inspect the file again when it finishes."));
        co_return;
    }
    const ProviderModule *old = m_registry->module(manifest.id);
    if (old && ProviderPackage::compareVersions(manifest.version, old->manifest.version) <= 0) {
        failInspection(
            requestId, QStringLiteral("A local-file update must have a newer version than the installed provider."));
        co_return;
    }
    const QStringList oldCapabilities = old ? old->manifest.capabilities : QStringList {};
    const QStringList oldOrigins = old ? old->manifest.origins : QStringList {};
    const auto difference = [](const QStringList& left, const QStringList& right) {
        QStringList result;
        for (const QString& value : left) {
            if (!right.contains(value) && !result.contains(value))
                result.append(value);
        }
        return result;
    };
    auto inspection = std::make_unique<Inspection>();
    inspection->token = QUuid::createUuid().toString(QUuid::Id128);
    inspection->digest = std::move(result.digest);
    inspection->registryRevision = revision;
    inspection->generation = generation;
    inspection->origin = m_origins.value(manifest.id);
    inspection->preview = { { QStringLiteral("token"), inspection->token },
        { QStringLiteral("sha256"), inspection->digest }, { QStringLiteral("id"), manifest.id },
        { QStringLiteral("name"), manifest.name }, { QStringLiteral("summary"), manifest.summary },
        { QStringLiteral("publisher"), manifest.publisher.left(256) }, { QStringLiteral("publisherVerified"), false },
        { QStringLiteral("version"), manifest.version },
        { QStringLiteral("installedVersion"), old ? old->manifest.version : QString() },
        { QStringLiteral("provenance"), QStringLiteral("local-file") },
        { QStringLiteral("installedProvenance"), provenance(manifest.id) },
        { QStringLiteral("isUpdate"), old != nullptr }, { QStringLiteral("capabilities"), manifest.capabilities },
        { QStringLiteral("origins"), manifest.origins },
        { QStringLiteral("addedCapabilities"), difference(manifest.capabilities, oldCapabilities) },
        { QStringLiteral("removedCapabilities"), difference(oldCapabilities, manifest.capabilities) },
        { QStringLiteral("addedOrigins"), difference(manifest.origins, oldOrigins) },
        { QStringLiteral("removedOrigins"), difference(oldOrigins, manifest.origins) },
        { QStringLiteral("warning"),
            old ? QStringLiteral("This local-file update is unverified and requires renewed trust. "
                                 "Its code can access saved account credentials and active accounts may restart. "
                                 "The claimed publisher and file hash do not authenticate the publisher.")
                : QStringLiteral("This local file is unverified. Installing trusts its JavaScript and QML code "
                                 "with credentials supplied to its accounts. The claimed publisher and file hash "
                                 "do not authenticate the publisher.") } };
    inspection->package = std::move(*result.package);
    m_inspection = std::move(inspection);
    const QVariantMap preview = m_inspection->preview;
    emit inspectedPackageChanged();
    emit fileInspectionFinished(requestId, preview, {});
}

void ProviderStore::installInspected(const QString& token)
{
    if (!linksAllowed() || !m_inspection || token.isEmpty() || token != m_inspection->token) {
        const QString error = QStringLiteral("Provider approval expired. Inspect the file again.");
        emit fileInstallationFinished(token, {}, error);
        emit problem(error);
        return;
    }
    const QString id = m_inspection->package.manifest.id;
    if (m_inspection->registryRevision != m_registryRevision || !permitsFileReplacement(id)
        || m_inspection->origin.channel != m_origins.value(id).channel
        || m_inspection->origin.feed != m_origins.value(id).feed || m_busy.contains(id)) {
        cancelInspection();
        const QString error
            = QStringLiteral("Installed providers changed. Inspect the file again before approving it.");
        emit fileInstallationFinished(token, id, error);
        emit problem(error);
        return;
    }
    auto inspection = std::move(m_inspection);
    m_fileInstalling = true;
    m_installingToken = inspection->token;
    m_transferProgress.insert(id,
        QVariantMap { { QStringLiteral("name"), inspection->package.manifest.name },
            { QStringLiteral("operationToken"), inspection->token }, { QStringLiteral("received"), 0 },
            { QStringLiteral("total"), -1 } });
    emit inspectedPackageChanged();
    setBusy(id, QStringLiteral("installing"));
    Async::runScoped(
        this, installInspectedAsync(std::move(inspection)), [] { }, [](const std::exception_ptr&) { },
        "provider file install");
}

QCoro::Task<void> ProviderStore::installInspectedAsync(std::unique_ptr<Inspection> inspection)
{
    QPointer<ProviderStore> guard(this);
    const QString id = inspection->package.manifest.id;
    const QString name = inspection->package.manifest.name;
    const QString token = inspection->token;
    const quint64 revision = inspection->registryRevision;
    const quint64 generation = inspection->generation;
    const Origin origin = inspection->origin;
    const auto admitted = [guard, revision, generation, id, origin] {
        return guard && guard->m_registry && guard->linksAllowed() && guard->m_registryRevision == revision
            && guard->m_inspectionGeneration == generation && guard->permitsFileReplacement(id)
            && guard->m_origins.value(id).channel == origin.channel && guard->m_origins.value(id).feed == origin.feed;
    };
    const auto progress = [guard, token, id](qint64 written, qint64 total) {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [guard, token, id, written, total] {
                if (!guard || guard->m_installingToken != token || !guard->m_fileInstalling)
                    return;
                auto row = guard->m_transferProgress.value(id).toMap();
                if (row.value(QStringLiteral("operationToken")).toString() != token)
                    return;
                row.insert(QStringLiteral("received"), written);
                row.insert(QStringLiteral("total"), total);
                guard->m_transferProgress.insert(id, row);
                emit guard->busyChanged();
            },
            Qt::QueuedConnection);
    };
    try {
        co_await m_registry->install(std::move(inspection->package), admitted, progress);
        if (!guard)
            co_return;
        m_origins.insert(id, { QStringLiteral("file"), {} });
        saveOrigins();
        m_fileInstalling = false;
        setBusy(id, {});
        m_installingToken.clear();
        m_inspectionRequestId.clear();
        emit fileInstallationFinished(token, id, {});
        emit installed(id, name);
    } catch (const std::exception&) {
        if (!guard)
            co_return;
        m_fileInstalling = false;
        setBusy(id, {});
        m_installingToken.clear();
        m_inspectionRequestId.clear();
        const QString error = QStringLiteral("Couldn't install that provider file. Inspect it again before retrying.");
        emit fileInstallationFinished(token, id, error);
        emit problem(error);
    }
}

bool ProviderStore::allows(const Origin& origin) const
{
    switch (m_sources) {
    case ProviderSources::Bundled:
        return false;
    case ProviderSources::Curated:
        return origin.channel == QStringLiteral("official") || origin.channel == QStringLiteral("community");
    case ProviderSources::Open:
        return true;
    }
    return false;
}

QCoro::Task<void> ProviderStore::installEntry(QVariantMap entry, Origin origin)
{
    const QString id = text(entry, "id");
    const QString name = text(entry, "name");
    // Every download comes through here, so this is the one gate.
    if (!allows(origin)) {
        qWarning("providers: %s is not from a source this build installs from", qPrintable(id));
        co_return;
    }
    if (m_busy.contains(id)) {
        emit problem(QStringLiteral("That provider is already being changed. Please wait."));
        co_return;
    }
    if (entry.value(QStringLiteral("format")).toInt() != 3) {
        emit problem(QStringLiteral("%1 needs a different version of Spool").arg(name));
        co_return;
    }
    QPointer<ProviderStore> guard(this);
    m_transferProgress.insert(id,
        QVariantMap {
            { QStringLiteral("name"), name }, { QStringLiteral("received"), 0 }, { QStringLiteral("total"), -1 } });
    setBusy(id, QStringLiteral("downloading"));
    try {
        const QByteArray archive = co_await fetch(QUrl(text(entry, "url")), kPackageLimit, id);
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
        co_await m_registry->install(*package, [guard] { return guard && guard->m_registry; });
        if (!guard)
            co_return;
        co_await loadOrigins();
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
    if (!storeAvailable())
        return;
    Async::runScoped(this, checkAsync(policy), [] { }, [](const std::exception_ptr&) { }, "provider update check");
}

QCoro::Task<void> ProviderStore::checkAsync(QString policy)
{
    QPointer<ProviderStore> guard(this);
    co_await loadOrigins();
    if (!guard)
        co_return;
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
        if (!allows(origin))
            continue;
        if (origin.channel == QStringLiteral("file"))
            continue; // Unverified local updates always require a fresh inspection and consent.
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
        if (module && validEntry(latest) && latest.value(QStringLiteral("format")).toInt() == 3
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

} // namespace Spool
