#include "provider/ProviderStore.h"
#include "ProviderFixture.h"
#include "TestMain.h"
#include "cache/DatabaseManager.h"
#include "provider/ProviderRegistry.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

#include <cstdlib>
#include <functional>
#include <iostream>

using namespace Spool;

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void waitUntil(const std::function<bool()>& condition, const char *message)
{
    QElapsedTimer timeout;
    timeout.start();
    while (!condition() && timeout.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(condition(), message);
}

// The store only follows https links; this sends every one of them to the
// local fixture server instead, as http://127.0.0.1:<port>/<host><path>.
class LocalNetwork final : public QNetworkAccessManager {
public:
    quint16 port = 0;

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest& request, QIODevice *data) override
    {
        QNetworkRequest local(request);
        QUrl url = request.url();
        url.setPath(QLatin1Char('/') + url.host() + url.path());
        url.setScheme(QStringLiteral("http"));
        url.setHost(QStringLiteral("127.0.0.1"));
        url.setPort(port);
        local.setUrl(url);
        return QNetworkAccessManager::createRequest(operation, local, data);
    }
};

QVariantMap entryFor(const QByteArray& archive, const QString& id, const QString& version, const QString& url)
{
    return { { QStringLiteral("id"), id }, { QStringLiteral("name"), QStringLiteral("Fixture ") + id },
        { QStringLiteral("version"), version }, { QStringLiteral("format"), 3 }, { QStringLiteral("url"), url },
        { QStringLiteral("sha256"),
            QString::fromLatin1(QCryptographicHash::hash(archive, QCryptographicHash::Sha256).toHex()) } };
}

QByteArray catalog(const QVariantList& entries)
{
    return QJsonDocument(QJsonObject { { QStringLiteral("providers"), QJsonArray::fromVariantList(entries) } })
        .toJson();
}

bool listed(const QVariantList& entries, const QString& id, const char *flag = nullptr)
{
    for (const QVariant& value : entries) {
        const QVariantMap entry = value.toMap();
        if (entry.value(QStringLiteral("id")) == id)
            return !flag || entry.value(QLatin1String(flag)).toBool();
    }
    return false;
}

} // namespace

SPOOL_TEST_MAIN("provider-store")
{
    QCoreApplication app(argc, argv);

    struct Case {
        const char *input;
        const char *feed;
    };
    for (const Case& row : std::initializer_list<Case> {
             { "github.com/spool-player/spool-jellyfin",
                 "https://github.com/spool-player/spool-jellyfin/releases/latest/download/spool-provider.json" },
             { "https://github.com/o/r.git", "https://github.com/o/r/releases/latest/download/spool-provider.json" },
             { "https://github.com/o/r/tree/main/logic",
                 "https://github.com/o/r/releases/latest/download/spool-provider.json" },
             { "https://github.com/o/r/releases/download/v1.0.0/o.r-1.0.0.tar.zst",
                 "https://github.com/o/r/releases/download/v1.0.0/spool-provider.json" },
             { "https://github.com/o/r/releases/download/v1.0.0/o.r-1.0.0.szo",
                 "https://github.com/o/r/releases/download/v1.0.0/spool-provider.json" },
             { "https://gitlab.com/group/sub/project",
                 "https://gitlab.com/group/sub/project/-/releases/permalink/latest/downloads/spool-provider.json" },
             { "https://gitlab.com/group/project/-/releases",
                 "https://gitlab.com/group/project/-/releases/permalink/latest/downloads/spool-provider.json" },
             { "https://gitlab.example.org/team/provider",
                 "https://gitlab.example.org/team/provider/-/releases/permalink/latest/downloads/spool-provider.json" },
             { "http://example.org/feeds/mine.json", "https://example.org/feeds/mine.json" },
             { "example.org", "https://example.org/spool-provider.json" },
             { "https://user.github.io/provider/", "https://user.github.io/provider/spool-provider.json" },
             { "", "" },
         }) {
        const QString actual = ProviderStore::feedUrlFor(QLatin1String(row.input)).toString();
        if (actual != QLatin1String(row.feed)) {
            std::cerr << row.input << " -> " << actual.toStdString() << '\n';
            require(false, "a link resolves to where its feed is published");
        }
    }

    QHash<QByteArray, QByteArray> routes;
    QList<QByteArray> requested;
    int requests = 0;
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost), "fixture server listens");
    QObject::connect(&server, &QTcpServer::newConnection, &app, [&] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &routes, &requested, &requests] {
                const QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n"))
                    return;
                ++requests;
                const QByteArray path = request.split(' ').value(1);
                requested.append(path);
                const bool found = routes.contains(path);
                const QByteArray body = routes.value(path);
                socket->write((found ? "HTTP/1.1 200 OK" : "HTTP/1.1 404 Not Found")
                    + QByteArray("\r\nConnection: close\r\nContent-Length: ") + QByteArray::number(body.size())
                    + "\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        }
    });
    LocalNetwork network;
    network.port = server.serverPort();

    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    qputenv("SPOOL_CREDENTIAL_STORE_DIR", directory.filePath(QStringLiteral("credentials")).toUtf8());
    DatabaseManager database;
    require(database.initialize(directory.filePath(QStringLiteral("cache.sqlite"))), "database opens");
    ProviderRegistry registry(&database);
    registry.setInstallDirectory(directory.filePath(QStringLiteral("providers")));
    registry.loadModules();
    QCoro::waitFor(registry.restore());

    const QByteArray v1 = ProviderFixture::archive(ProviderFixture::package(QStringLiteral("fixture.test")));
    const QByteArray v2
        = ProviderFixture::archive(ProviderFixture::package(QStringLiteral("fixture.test"), QStringLiteral("1.1.0")));
    const QByteArray other = ProviderFixture::archive(ProviderFixture::package(QStringLiteral("fixture.other")));
    const QByteArray linked = ProviderFixture::archive(ProviderFixture::package(QStringLiteral("fixture.linked")));
    routes.insert("/store.test/v1.tar.zst", v1);
    routes.insert("/store.test/v2.tar.zst", v2);
    routes.insert("/store.test/other.tar.zst", other);
    routes.insert("/feed.test/linked.tar.zst", linked);
    QVariantMap tampered = entryFor(other, QStringLiteral("fixture.bad"), QStringLiteral("1.0.0"),
        QStringLiteral("https://store.test/other.tar.zst"));
    routes.insert("/store.test/official.json",
        catalog({ entryFor(v1, QStringLiteral("fixture.test"), QStringLiteral("1.0.0"),
                      QStringLiteral("https://store.test/v1.tar.zst")),
            tampered,
            QVariantMap { { QStringLiteral("id"), QStringLiteral("insecure") },
                { QStringLiteral("name"), QStringLiteral("x") }, { QStringLiteral("version"), QStringLiteral("1.0.0") },
                { QStringLiteral("url"), QStringLiteral("http://x/y") },
                { QStringLiteral("sha256"), QString(64, QLatin1Char('0')) } } }));
    routes.insert("/store.test/index.json",
        catalog({ entryFor(other, QStringLiteral("fixture.other"), QStringLiteral("1.0.0"),
            QStringLiteral("https://store.test/other.tar.zst")) }));
    routes.insert("/feed.test/spool-provider.json",
        QJsonDocument(QJsonObject::fromVariantMap(entryFor(linked, QStringLiteral("fixture.linked"),
                          QStringLiteral("1.0.0"), QStringLiteral("https://feed.test/linked.tar.zst"))))
            .toJson());

    QStringList installed;
    QStringList problems;
    {
        ProviderStore store(&registry, &database, &network, QUrl(QStringLiteral("https://store.test/")));
        QObject::connect(
            &store, &ProviderStore::installed, [&](const QString& id, const QString&) { installed.append(id); });
        QObject::connect(&store, &ProviderStore::problem, [&](const QString& message) { problems.append(message); });
        require(listed(store.official(), QStringLiteral("spool.jellyfin"), "installed"),
            "bundled providers are listed before the store answers");

        store.refresh(true);
        waitUntil([&] { return !store.loading(); }, "the catalogue loads");
        require(store.error().isEmpty(), "the catalogue loads without error");
        require(listed(store.official(), QStringLiteral("fixture.test"), "compatible")
                && !listed(store.official(), QStringLiteral("insecure")),
            "entries without an https link and a digest are skipped");
        require(listed(store.community(), QStringLiteral("fixture.other")), "community providers are listed");

        QVariantList transferHistory;
        QObject::connect(&store, &ProviderStore::busyChanged, [&] {
            for (const auto& transfer : store.transfers())
                transferHistory.append(transfer);
        });
        store.install(QStringLiteral("fixture.test"));
        waitUntil([&] { return installed.contains(QStringLiteral("fixture.test")); }, "a store install completes");
        require(registry.module(QStringLiteral("fixture.test")), "and registers the provider");
        bool downloaded = false, installing = false;
        for (const auto& transfer : transferHistory) {
            const auto row = transfer.toMap();
            require(row.value("id") == "fixture.test" && row.value("name") == "Fixture fixture.test",
                "transfer progress belongs to the selected provider");
            downloaded
                |= row.value("received").toLongLong() == v1.size() && row.value("total").toLongLong() == v1.size();
            installing |= row.value("state") == "installing";
        }
        require(downloaded && installing && store.transfers().isEmpty(),
            "download byte progress reaches validation/installation then closes on success");
        require(
            listed(store.official(), QStringLiteral("fixture.test"), "installed"), "the listing shows it installed");

        store.install(QStringLiteral("fixture.bad"));
        waitUntil([&] { return !problems.isEmpty(); }, "a mismatching download is reported");
        require(
            problems.last().contains(QStringLiteral("didn't match")) && !registry.module(QStringLiteral("fixture.bad")),
            "a download whose digest or identity differs is never installed");

        require(store.transfers().isEmpty(), "failed validation closes the progress popup too");
        store.install(QStringLiteral("fixture.other"));
        waitUntil([&] { return installed.contains(QStringLiteral("fixture.other")); }, "a community install completes");

        store.addFromUrl(QStringLiteral("feed.test"));
        waitUntil(
            [&] { return installed.contains(QStringLiteral("fixture.linked")); }, "a provider installs from a link");
        require(
            listed(store.community(), QStringLiteral("fixture.linked"), "fromUrl"), "and is listed as added by link");
        problems.clear();
        store.addFromUrl(QStringLiteral("https://nowhere.test/"));
        waitUntil([&] { return !problems.isEmpty(); }, "a missing feed is reported");
        require(problems.last().contains(QStringLiteral("No spool-provider.json")), "as a missing feed");
    }

    // A fresh store (a later launch) installing before its update check must
    // keep what the previous one remembered about where providers came from.
    routes.insert("/store.test/official.json",
        catalog({ entryFor(v2, QStringLiteral("fixture.test"), QStringLiteral("1.1.0"),
            QStringLiteral("https://store.test/v2.tar.zst")) }));
    {
        ProviderStore store(&registry, &database, &network, QUrl(QStringLiteral("https://store.test/")));
        store.uninstall(QStringLiteral("fixture.other"));
        waitUntil([&] { return !registry.module(QStringLiteral("fixture.other")); }, "a provider can be removed");
        waitUntil([&] { return listed(store.community(), QStringLiteral("fixture.linked"), "fromUrl"); },
            "providers added by link are remembered across launches");
        QCoro::waitFor(registry.install(ProviderFixture::package(QStringLiteral("fixture.manual"))));
        require(listed(store.community(), QStringLiteral("fixture.manual"), "installed"),
            "an installed provider is listed even with no catalogue entry for it");

        store.checkForUpdates(QStringLiteral("ask"));
        waitUntil([&] { return store.updates().size() == 1; }, "a newer official release is offered");
        require(registry.module(QStringLiteral("fixture.test"))->manifest.version == QStringLiteral("1.0.0"),
            "asking does not install");
        installed.clear();
        QObject::connect(
            &store, &ProviderStore::installed, [&](const QString& id, const QString&) { installed.append(id); });
        store.checkForUpdates(QStringLiteral("auto"));
        waitUntil([&] { return installed.contains(QStringLiteral("fixture.test")); }, "automatic updates install");
        require(registry.module(QStringLiteral("fixture.test"))->manifest.version == QStringLiteral("1.1.0")
                && store.updates().isEmpty(),
            "the new version replaces the old and the offer goes away");
    }

    // A curated build (Google Play, the App Store) installs from the store's
    // catalogues and never follows a provider's own link.
    const QByteArray linkedV2
        = ProviderFixture::archive(ProviderFixture::package(QStringLiteral("fixture.linked"), QStringLiteral("1.1.0")));
    routes.insert("/feed.test/linked2.tar.zst", linkedV2);
    routes.insert("/feed.test/spool-provider.json",
        QJsonDocument(QJsonObject::fromVariantMap(entryFor(linkedV2, QStringLiteral("fixture.linked"),
                          QStringLiteral("1.1.0"), QStringLiteral("https://feed.test/linked2.tar.zst"))))
            .toJson());
    const auto checked = [&](ProviderStore& store) {
        bool done = false;
        const auto connection = QObject::connect(&store, &ProviderStore::updatesChanged, [&] { done = true; });
        store.checkForUpdates(QStringLiteral("ask"));
        waitUntil([&] { return done; }, "the update check finishes");
        QObject::disconnect(connection);
    };
    {
        ProviderStore store(
            &registry, &database, &network, QUrl(QStringLiteral("https://store.test/")), ProviderSources::Curated);
        require(store.storeAvailable() && !store.linksAllowed(), "a curated build has the store and no links");
        problems.clear();
        QObject::connect(&store, &ProviderStore::problem, [&](const QString& message) { problems.append(message); });
        requested.clear();
        store.addFromUrl(QStringLiteral("feed.test"));
        require(problems.size() == 1
                && problems.last().contains(QStringLiteral("only installs providers from its store"))
                && requested.isEmpty(),
            "adding by link is refused without a request");
        checked(store);
        require(!requested.contains("/feed.test/spool-provider.json") && store.updates().isEmpty(),
            "a provider added by link is not updated from its link");
        store.refresh(true);
        waitUntil([&] { return !store.loading(); }, "the curated catalogue loads");
        require(listed(store.official(), QStringLiteral("fixture.test"), "installed"), "and lists the store");
    }
    {
        ProviderStore store(&registry, &database, &network, QUrl(QStringLiteral("https://store.test/")));
        checked(store);
        require(store.updates().size() == 1, "an open build does follow the link, so the check above meant something");
    }

    // A bundled build has no store and loads nothing from disk, even with
    // providers installed there by an earlier build.
    {
        ProviderRegistry bundledRegistry(&database);
        bundledRegistry.loadModules();
        require(bundledRegistry.module(QStringLiteral("spool.jellyfin"))
                && !bundledRegistry.module(QStringLiteral("fixture.test")),
            "a bundled build runs only its own providers");
        ProviderStore store(&bundledRegistry, &database, &network, QUrl(QStringLiteral("https://store.test/")),
            ProviderSources::Bundled);
        require(!store.storeAvailable() && !store.linksAllowed(), "a bundled build has no store");
        requested.clear();
        store.refresh(true);
        store.checkForUpdates(QStringLiteral("auto"));
        store.addFromUrl(QStringLiteral("feed.test"));
        QCoreApplication::processEvents();
        require(!store.loading() && requested.isEmpty(), "and asks the network for nothing");
        require(listed(store.official(), QStringLiteral("spool.jellyfin"), "installed"),
            "its bundled providers are still listed");
        bool refused = false;
        try {
            QCoro::waitFor(bundledRegistry.install(ProviderFixture::package(QStringLiteral("fixture.manual"))));
        } catch (const std::exception&) {
            refused = true;
        }
        require(refused, "and its registry has nowhere to install to");
    }
    {
        ProviderStore store(&registry, &database, &network, QUrl(QStringLiteral("https://store.test/")));
        problems.clear();
        QObject::connect(&store, &ProviderStore::problem, [&](const QString& message) { problems.append(message); });
        QObject::connect(
            &store, &ProviderStore::installed, [&](const QString& id, const QString&) { installed.append(id); });
        QHash<QString, QVariantMap> inspectionOutcomes;
        QHash<QString, QVariantMap> installationOutcomes;
        QObject::connect(&store, &ProviderStore::fileInspectionFinished,
            [&](const QString& request, const QVariantMap& preview, const QString& error) {
                require(!inspectionOutcomes.contains(request), "inspection completion is exactly once per request");
                inspectionOutcomes.insert(request, { { "preview", preview }, { "error", error } });
            });
        QObject::connect(&store, &ProviderStore::fileInstallationFinished,
            [&](const QString& token, const QString& moduleId, const QString& error) {
                installationOutcomes.insert(token, { { "id", moduleId }, { "error", error } });
            });
        QVariantList localTransfers;
        QObject::connect(&store, &ProviderStore::busyChanged, [&] {
            for (const QVariant& value : store.transfers()) {
                if (!value.toMap().value("operationToken").toString().isEmpty())
                    localTransfers.append(value);
            }
        });
        const QString path = directory.filePath(QStringLiteral("review.szo"));
        const QUrl fileUrl = QUrl::fromLocalFile(path);
        const auto writeFile = [&](const QByteArray& bytes) {
            QFile file(path);
            require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(),
                "the local package fixture is written");
        };
        const auto inspect = [&](const QByteArray& bytes) {
            writeFile(bytes);
            const QString request = store.inspectFile(fileUrl);
            require(
                !inspectionOutcomes.contains(request), "inspection result arrives after its request ID is returned");
            store.addFromUrl(QString()); // Unrelated global error must not complete this request.
            waitUntil([&] { return inspectionOutcomes.contains(request); }, "scoped local inspection completes");
            return inspectionOutcomes.value(request).value("preview").toMap();
        };
        const QString id = QStringLiteral("fixture.file");
        const QByteArray first = ProviderFixture::archive(ProviderFixture::package(id));
        const QByteArray second = ProviderFixture::archive(ProviderFixture::package(id, QStringLiteral("1.1.0")));
        const QVariantMap preview = inspect(first);
        const QString token = preview.value("token").toString();
        require(!token.isEmpty() && preview.value("publisherVerified") == false
                && preview.value("provenance") == "local-file" && preview.value("installedVersion").toString().isEmpty()
                && preview.value("sha256").toString()
                    == QString::fromLatin1(QCryptographicHash::hash(first, QCryptographicHash::Sha256).toHex())
                && !registry.module(id) && !QFileInfo::exists(QDir(registry.installDirectory()).filePath(id)),
            "inspection binds actual bytes without registering code or writing an installed package");
        store.installInspected(QStringLiteral("not-the-token"));
        require(!registry.module(id) && store.inspectedPackage().value("token") == token,
            "a guessed token cannot authorize installation");
        store.cancelInspection(QStringLiteral("unrelated-operation"));
        require(store.inspectedPackage().value("token") == token, "another operation cannot cancel this consent");
        store.cancelInspection(token);
        store.installInspected(token);
        require(
            store.inspectedPackage().isEmpty() && !registry.module(id), "cancelled consent cannot later be replayed");

        const QString approved = inspect(first).value("token").toString();
        writeFile(second);
        store.installInspected(approved);
        store.addFromUrl(QString());
        require(!installationOutcomes.contains(approved), "an unrelated global error cannot finish a local install");
        waitUntil([&] { return installed.contains(id); }, "an approved local file installs");
        require(installationOutcomes.value(approved).value("id") == id
                && installationOutcomes.value(approved).value("error").toString().isEmpty(),
            "local installation success carries the approved token and module identity");
        qint64 expectedBytes = 0;
        const auto expectedPackage = ProviderFixture::package(id);
        for (const QByteArray& bytes : expectedPackage.files)
            expectedBytes += bytes.size();
        bool completedBytes = false;
        for (const QVariant& value : localTransfers) {
            const QVariantMap row = value.toMap();
            if (row.value("operationToken") != approved)
                continue;
            require(row.value("id") == id && row.value("state") == "installing",
                "local transfer progress stays correlated with the approved provider");
            completedBytes |= row.value("received").toLongLong() == expectedBytes
                && row.value("total").toLongLong() == expectedBytes;
        }
        require(completedBytes && store.transfers().isEmpty(),
            "real staged-file byte progress reaches completion and the transfer closes");
        require(registry.module(id)->manifest.version == "1.0.0",
            "changing the pathname after inspection cannot swap in unapproved bytes");
        QFile installedManifest(registry.module(id)->file(QStringLiteral("manifest.json")).toLocalFile());
        require(installedManifest.open(QIODevice::ReadOnly)
                && QJsonDocument::fromJson(installedManifest.readAll()).object().value("version").toString() == "1.0.0",
            "the immutable approved version reaches disk, not just the module listing");

        const QVariantMap update = inspect(second);
        require(update.value("isUpdate").toBool() && update.value("installedVersion") == "1.0.0"
                && update.value("version") == "1.1.0" && update.value("installedProvenance") == "file"
                && update.value("warning").toString().contains(QStringLiteral("renewed trust")),
            "a file-provider update displays old/new versions and renewed credential trust");
        installed.removeAll(id);
        store.installInspected(update.value("token").toString());
        waitUntil([&] { return installed.contains(id); }, "renewed consent installs a local update");
        require(registry.module(id)->manifest.version == "1.1.0", "the approved local update is active");
        require(inspect(first).isEmpty() && registry.module(id)->manifest.version == "1.1.0",
            "a stale local version cannot delete the active newer installation");

        store.refresh(true);
        waitUntil([&] { return !store.loading(); }, "takeover catalogue loads");
        installed.removeAll(QStringLiteral("fixture.other"));
        store.install(QStringLiteral("fixture.other"));
        waitUntil([&] { return installed.contains(QStringLiteral("fixture.other")); }, "community identity exists");
        for (const QString& trusted :
            { QStringLiteral("spool.jellyfin"), QStringLiteral("fixture.test"), QStringLiteral("fixture.other"),
                QStringLiteral("fixture.linked"), QStringLiteral("fixture.manual") }) {
            const QString version = registry.module(trusted)->manifest.version;
            auto takeover = ProviderFixture::package(trusted, QStringLiteral("999.0.0"));
            QJsonObject manifest = QJsonDocument::fromJson(takeover.files.value("manifest.json")).object();
            manifest.insert("publisher", registry.module(trusted)->manifest.publisher);
            takeover.files["manifest.json"] = QJsonDocument(manifest).toJson();
            require(inspect(ProviderFixture::archive(takeover)).isEmpty()
                    && registry.module(trusted)->manifest.version == version
                    && problems.last().contains(QStringLiteral("unverified local file")),
                "matching identity and publisher claims never authorize trusted-distribution takeover");
        }

        const QString cancelledId = QStringLiteral("fixture.cancelled");
        const QString cancelled
            = inspect(ProviderFixture::archive(ProviderFixture::package(cancelledId))).value("token").toString();
        store.installInspected(cancelled);
        store.cancelInspection();
        waitUntil([&] { return !store.busy().contains(cancelledId); }, "cancelled staging settles");
        require(!registry.module(cancelledId)
                && !QFileInfo::exists(QDir(registry.installDirectory()).filePath(cancelledId + "/1.0.0")),
            "cancellation during async staging never activates candidate bytes");

        const QString revisedId = QStringLiteral("fixture.revised");
        const QString revised
            = inspect(ProviderFixture::archive(ProviderFixture::package(revisedId))).value("token").toString();
        QCoro::waitFor(registry.install(ProviderFixture::package(revisedId, QStringLiteral("1.1.0"))));
        store.installInspected(revised);
        require(store.inspectedPackage().isEmpty() && registry.module(revisedId)->manifest.version == "1.1.0",
            "replacement after preview invalidates consent before stale activation");

        writeFile(QByteArrayLiteral("not a package"));
        QHash<QString, QVariantMap> classifications;
        QObject::connect(&store, &ProviderStore::filesClassified,
            [&](const QString& request, const QVariantList& packages, const QVariantList& media, const QString& error) {
                classifications.insert(request, { { "packages", packages }, { "media", media }, { "error", error } });
            });
        const auto classify = [&](const QVariantList& urls) {
            const QString request = store.classifyFiles(urls);
            require(!classifications.contains(request), "classification completion follows returned request ownership");
            waitUntil([&] { return classifications.contains(request); }, "scoped file classification completes");
            return classifications.value(request);
        };
        require(store.isPackageCandidate(fileUrl), "a malformed dedicated-suffix file stays a package candidate");
        require(inspect(QByteArrayLiteral("not a package")).isEmpty(), "malformed candidates report an error");
        const QString unlabeledPath = directory.filePath(QStringLiteral("unlabeled.bin"));
        require(QFile::copy(path, unlabeledPath), "unlabeled negative fixture is copied");
        require(classify({ QUrl::fromLocalFile(unlabeledPath) }).value("media").toList().size() == 1,
            "ordinary bytes are classified as media without package filename guessing");
        writeFile(first);
        QFile::remove(unlabeledPath);
        require(QFile::copy(path, unlabeledPath)
                && classify({ QUrl::fromLocalFile(unlabeledPath) }).value("packages").toList().size() == 1,
            "asynchronous bounded content sniff recognizes an unlabeled provider archive");
#ifdef Q_OS_UNIX
        const QString fifoPath = directory.filePath(QStringLiteral("special.pipe"));
        require(::mkfifo(QFile::encodeName(fifoPath).constData(), 0600) == 0, "native FIFO fixture is created");
        const QUrl fifoUrl = QUrl::fromLocalFile(fifoPath);
        require(!store.isPackageCandidate(fifoUrl), "cheap filename hint never opens a special file");
        bool heartbeat = false;
        QTimer::singleShot(0, &app, [&] { heartbeat = true; });
        const QVariantMap special = classify({ fifoUrl });
        require(heartbeat && !special.value("error").toString().isEmpty()
                && special.value("packages").toList().isEmpty() && special.value("media").toList().isEmpty(),
            "a real FIFO is rejected off-thread without blocking GUI events or falling back to media");
        const QString specialRequest = store.inspectFile(fifoUrl);
        waitUntil(
            [&] { return inspectionOutcomes.contains(specialRequest); }, "special-file inspection rejects safely");
        require(!inspectionOutcomes.value(specialRequest).value("error").toString().isEmpty(),
            "the consent inspection path also refuses nonregular files before opening them");
#endif
    }

    {
        // Registry lifecycle risks differ from the Store's token checks.
        const QString id = QStringLiteral("fixture.file");
        bool approved = true;
        auto revoked
            = registry.install(ProviderFixture::package(id, QStringLiteral("1.2.0")), [&approved] { return approved; });
        approved = false;
        bool cancelled = false;
        try {
            QCoro::waitFor(std::move(revoked));
        } catch (const std::exception&) {
            cancelled = true;
        }
        require(cancelled && registry.module(id)->manifest.version == "1.1.0"
                && QFileInfo::exists(QDir(registry.installDirectory()).filePath(id + "/1.1.0"))
                && !QFileInfo::exists(QDir(registry.installDirectory()).filePath(id + "/1.2.0")),
            "revoked admission after staging preserves installed bytes and module identity");

        auto stale = registry.install(ProviderFixture::package(id, QStringLiteral("1.2.0")));
        QCoro::waitFor(registry.install(ProviderFixture::package(id, QStringLiteral("1.3.0"))));
        cancelled = false;
        try {
            QCoro::waitFor(std::move(stale));
        } catch (const std::exception&) {
            cancelled = true;
        }
        require(cancelled && registry.module(id)->manifest.version == "1.3.0"
                && !QFileInfo::exists(QDir(registry.installDirectory()).filePath(id + "/1.2.0")),
            "a superseded registry transaction cannot activate stale staged bytes");

        auto owner = std::make_unique<ProviderRegistry>(&database);
        const QString root = directory.filePath(QStringLiteral("abandoned-owner"));
        owner->setInstallDirectory(root);
        auto abandoned = owner->install(ProviderFixture::package(QStringLiteral("fixture.abandoned")));
        owner.reset();
        cancelled = false;
        try {
            QCoro::waitFor(std::move(abandoned));
        } catch (const std::exception&) {
            cancelled = true;
        }
        require(cancelled && !QFileInfo::exists(QDir(root).filePath(QStringLiteral("fixture.abandoned/1.0.0"))),
            "destroying the registry during staging cannot publish a package");
    }

    for (ProviderSources policy : { ProviderSources::Curated, ProviderSources::Bundled }) {
        ProviderStore store(&registry, &database, &network, QUrl(QStringLiteral("https://store.test/")), policy);
        int rejected = 0;
        QObject::connect(&store, &ProviderStore::problem, [&](const QString&) { ++rejected; });
        store.inspectFile(QUrl::fromLocalFile(directory.filePath(QStringLiteral("review.szo"))));
        waitUntil([&] { return rejected == 1; }, "restricted policy returns its queued inspection error");
        require(rejected == 1 && store.inspectedPackage().isEmpty() && store.busy().isEmpty(),
            "restricted source policies reject local inspection without any code or disk work");
    }
    return 0;
}
