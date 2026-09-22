#include "provider/ProviderStore.h"
#include "ProviderFixture.h"
#include "TestMain.h"
#include "cache/DatabaseManager.h"
#include "provider/ProviderRegistry.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include <cstdlib>
#include <functional>
#include <iostream>

using namespace JellyfinNative;

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
        { QStringLiteral("version"), version }, { QStringLiteral("api"), QStringLiteral("0.2") },
        { QStringLiteral("url"), url },
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

JELLYFIN_TEST_MAIN("provider-store")
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
    int requests = 0;
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost), "fixture server listens");
    QObject::connect(&server, &QTcpServer::newConnection, &app, [&] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &routes, &requests] {
                const QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n"))
                    return;
                ++requests;
                const QByteArray path = request.split(' ').value(1);
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
    qputenv("JELLYFIN_CREDENTIAL_STORE_DIR", directory.filePath(QStringLiteral("credentials")).toUtf8());
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

        store.install(QStringLiteral("fixture.test"));
        waitUntil([&] { return installed.contains(QStringLiteral("fixture.test")); }, "a store install completes");
        require(registry.module(QStringLiteral("fixture.test")), "and registers the provider");
        require(
            listed(store.official(), QStringLiteral("fixture.test"), "installed"), "the listing shows it installed");

        store.install(QStringLiteral("fixture.bad"));
        waitUntil([&] { return !problems.isEmpty(); }, "a mismatching download is reported");
        require(
            problems.last().contains(QStringLiteral("didn't match")) && !registry.module(QStringLiteral("fixture.bad")),
            "a download whose digest or identity differs is never installed");

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
    return 0;
}
