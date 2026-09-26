#include "TestMain.h"
#include "cache/DatabaseManager.h"
#include "provider/PlaybackSource.h"
#include "provider/ProviderRegistry.h"
#include "provider/ProviderUiContext.h"
#include "provider/SourceHub.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <QThread>

#include <QCoroNetworkReply>

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

void waitUntil(const std::function<bool()>& condition, const char *message, int milliseconds = 10000)
{
    QElapsedTimer timeout;
    timeout.start();
    while (!condition() && timeout.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    require(condition(), message);
}

// Another Jellyfin client, for the things only a second client can do.
QByteArray admin(QNetworkAccessManager& network, const QByteArray& method, const QString& url, const QString& token,
    const QByteArray& body = {})
{
    QNetworkRequest request { QUrl(url) };
    request.setRawHeader("Authorization",
        "MediaBrowser Client=\"live-test\", Device=\"admin\", DeviceId=\"live-admin\", Version=\"1\", Token=\""
            + token.toUtf8() + '"');
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = network.sendCustomRequest(request, method, body);
    QCoro::waitFor(qCoro(reply).waitForFinished());
    reply->deleteLater();
    return reply->readAll();
}

} // namespace

// The bundled Jellyfin provider against a real server, end to end through the
// registry and hub. Opt-in: set SPOOL_LIVE_JELLYFIN to a server that has user
// SPOOL_LIVE_USER / SPOOL_LIVE_PASSWORD and a movies library.
SPOOL_TEST_MAIN("live-jellyfin")
{
    QCoreApplication app(argc, argv);
    const QString server = qEnvironmentVariable("SPOOL_LIVE_JELLYFIN");
    if (server.isEmpty()) {
        std::cout << "SPOOL_LIVE_JELLYFIN not set; skipped\n";
        return 0;
    }
    const QString user = qEnvironmentVariable("SPOOL_LIVE_USER", QStringLiteral("alice"));
    const QString password = qEnvironmentVariable("SPOOL_LIVE_PASSWORD");

    QTemporaryDir directory;
    qputenv("SPOOL_CREDENTIAL_STORE_DIR", directory.filePath(QStringLiteral("credentials")).toUtf8());
    DatabaseManager database;
    require(database.initialize(directory.filePath(QStringLiteral("cache.sqlite"))), "database opens");
    ProviderRegistry registry(&database);
    registry.setInstallDirectory(directory.filePath(QStringLiteral("providers")));
    registry.setRuntimeEnvironment({ { "id", "spool-live-test" }, { "name", "Spool live test" }, { "app", "Spool" },
                                       { "version", "0.0.0" }, { "platform", "linux" }, { "locale", "en" } },
        {});
    registry.loadModules();
    SourceHub hub(&registry);
    QList<QPair<QString, QVariantMap>> events;
    QObject::connect(&hub, &SourceHub::accountEvent,
        [&events](const QString&, const QString& type, const QVariantMap& p) { events.append({ type, p }); });
    QCoro::waitFor(registry.restore());

    // Sign in the way ui/Login.qml does.
    auto *login = qobject_cast<ProviderUiContext *>(registry.beginSetup(QStringLiteral("spool.jellyfin")));
    require(login, "Jellyfin needs a login screen");
    QCoro::waitFor(registry.allowSetupOrigin(login->sourceId(), QUrl(server)));
    const QVariantMap probe = QCoro::waitFor(
        registry.callSource(login->sourceId(), QStringLiteral("probe"), { { QStringLiteral("server"), server } }));
    require(!probe.value(QStringLiteral("id")).toString().isEmpty(), "probe identifies the server");
    std::cout << "server: " << probe.value(QStringLiteral("name")).toString().toStdString() << ' '
              << probe.value(QStringLiteral("version")).toString().toStdString() << '\n';
    bool rejected = false;
    try {
        QCoro::waitFor(registry.callSource(login->sourceId(), QStringLiteral("authenticate"),
            { { "server", server }, { "username", user }, { "password", password + "-wrong" } }));
    } catch (const std::exception& error) {
        rejected = true;
        std::cout << "wrong password: " << error.what() << '\n';
    }
    require(rejected, "a wrong password is refused");
    const QVariantMap account = QCoro::waitFor(registry.callSource(login->sourceId(), QStringLiteral("authenticate"),
        { { "server", server }, { "username", user }, { "password", password } }));
    require(
        account.value(QStringLiteral("account")).toString().contains(QLatin1Char('@')), "sign-in names the account");
    QString accountId;
    QObject::connect(&registry, &ProviderRegistry::accountAdded, [&accountId](const QString& id) { accountId = id; });
    login->complete(account);
    require(!accountId.isEmpty(), "the account is added");
    waitUntil([&] { return hub.source(accountId) != nullptr; }, "the account starts");
    const QString token = registry.accountList().front().configuration.value(QStringLiteral("token")).toString();

    // Catalogue.
    const auto libraries = QCoro::waitFor(hub.fetchLibraries());
    require(!libraries.empty(), "libraries are listed");
    const LibraryItem movies = libraries.front();
    std::cout << "library: " << movies.name.toStdString() << " (" << movies.collectionType.toStdString() << ")\n";
    const auto page = QCoro::waitFor(hub.fetchBrowsePage(BrowseDescriptor::library(movies.id, movies.collectionType)));
    require(!page.items.empty() && page.totalRecordCount == int(page.items.size()), "the library has items");
    for (const MovieItem& item : page.items)
        std::cout << "  " << item.title.toStdString() << " [" << item.id.toStdString() << "]\n";
    const MovieItem details = QCoro::waitFor(hub.fetchItemDetails(page.items.front().id));
    require(details.id == page.items.front().id && !details.mediaSources.empty(), "details carry the file");
    require(QCoro::waitFor(hub.searchItems(details.title.left(3))).size() >= 1, "search finds it");
    require(!QCoro::waitFor(hub.fetchLatestItems({}, 10)).empty(), "latest items");

    // Item state round trips through the server.
    QCoro::waitFor(hub.setItemFavorite(details.id, true));
    require(QCoro::waitFor(hub.fetchItemDetails(details.id)).favorite, "favourite is saved on the server");
    QCoro::waitFor(hub.setItemFavorite(details.id, false));
    QCoro::waitFor(hub.setItemPlaybackPosition(details.id, 10'000'000));
    require(QCoro::waitFor(hub.fetchItemDetails(details.id)).resumeTicks == 10'000'000, "resume point is saved");
    require(!QCoro::waitFor(hub.fetchResumeItems()).empty(), "and the item is resumable");

    // Playback: resolve, then fetch the stream the way mpv would.
    PlaybackSource *playback = hub.playback();
    const PlaybackSession session = QCoro::waitFor(playback->resolvePlayback(details, false));
    std::cout << "playback: " << session.playMethod.toStdString() << ' ' << session.container.toStdString() << '\n';
    require(session.itemId == details.id && !session.url.isEmpty(), "playback resolves");
    QNetworkAccessManager network;
    QNetworkRequest streamRequest { QUrl(session.url) };
    for (const QByteArray& line : playback->mediaRequestHeaders().split('\n')) {
        const qsizetype colon = line.indexOf(':');
        if (colon > 0)
            streamRequest.setRawHeader(line.left(colon).trimmed(), line.mid(colon + 1).trimmed());
    }
    QNetworkReply *stream = network.get(streamRequest);
    QCoro::waitFor(qCoro(stream).waitForFinished());
    const int status = stream->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    std::cout << "stream: HTTP " << status << ", " << stream->readAll().size() << " bytes\n";
    require(status == 200, "the stream is served with the session's credentials");
    QCoro::waitFor(playback->reportPlaybackStart(session, 1.0, 100, false));
    QCoro::waitFor(playback->reportPlaybackStopped(session, 20'000'000, false, 1.0));

    // Remote control and SyncPlay arrive over the provider's websocket.
    const QJsonArray sessions
        = QJsonDocument::fromJson(admin(network, "GET", server + "/Sessions?deviceId=spool-live-test", token)).array();
    require(!sessions.isEmpty(), "the server lists Spool's session");
    const QString sessionId = sessions.first().toObject().value(QStringLiteral("Id")).toString();
    const QJsonArray commands = sessions.first()
                                    .toObject()
                                    .value(QStringLiteral("Capabilities"))
                                    .toObject()
                                    .value(QStringLiteral("SupportedCommands"))
                                    .toArray();
    require(commands.contains(QStringLiteral("DisplayMessage")), "Spool registered what it can be asked to do");
    admin(network, "POST", server + "/Sessions/" + sessionId + "/Message", token,
        R"({"Text":"hello from the server","TimeoutMs":1000})");
    waitUntil(
        [&] {
            return std::any_of(events.begin(), events.end(), [](const auto& e) {
                return e.first == QStringLiteral("remote")
                    && e.second.value(QStringLiteral("command")) == QStringLiteral("message");
            });
        },
        "a message sent to the session arrives as a remote command");
    QCoro::waitFor(
        hub.call(accountId, QStringLiteral("groupCreate"), { { QStringLiteral("name"), QStringLiteral("Live") } }));
    waitUntil(
        [&] {
            return std::any_of(events.begin(), events.end(), [](const auto& e) {
                return e.first == QStringLiteral("group")
                    && e.second.value(QStringLiteral("type")) == QStringLiteral("joined");
            });
        },
        "creating a group joins it over the websocket");
    const QVariantMap groups = QCoro::waitFor(hub.call(accountId, QStringLiteral("groups")));
    require(!groups.value(QStringLiteral("items")).toList().isEmpty(), "the group is listed");
    const QVariantMap clock = QCoro::waitFor(hub.call(accountId, QStringLiteral("clock")));
    require(clock.value(QStringLiteral("sent")).toLongLong() > 0, "the server clock answers");
    QCoro::waitFor(hub.call(accountId, QStringLiteral("groupLeave")));

    // Item actions: a new playlist through the picker's answer.
    const QVariantMap added = QCoro::waitFor(hub.call(accountId, QStringLiteral("runItemAction"),
        { { "action", "playlist" }, { "itemId", SourceHub::rawId(details.id) }, { "itemType", "Movie" },
            { "newName", "Live test" } }));
    std::cout << "action: " << added.value(QStringLiteral("message")).toString().toStdString() << '\n';
    const QVariantMap targets
        = QCoro::waitFor(hub.call(accountId, QStringLiteral("targets"), { { "kind", "playlist" } }));
    require(!targets.value(QStringLiteral("items")).toList().isEmpty(), "the new playlist is a target");

    std::cout << "events:";
    for (const auto& event : events)
        std::cout << ' ' << event.first.toStdString() << '/'
                  << (event.second.value(QStringLiteral("type")).toString()
                         + event.second.value(QStringLiteral("command")).toString())
                         .toStdString();
    std::cout << "\nlive jellyfin ok\n";
    return 0;
}
