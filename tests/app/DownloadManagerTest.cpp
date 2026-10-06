#include "app/DownloadManager.h"
#include "../providers/ProviderFixture.h"
#include "TestMain.h"
#include "cache/DatabaseManager.h"
#include "provider/ProviderRegistry.h"
#include "provider/ProviderUiContext.h"
#include "provider/SourceHub.h"
#include "providers/local/LocalProvider.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <clocale>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mpv/client.h>

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
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(condition(), message);
}
QByteArray readFile(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "media file opens");
    return file.readAll();
}
void decodeOffline(const QString& url)
{
    mpv_handle *player = mpv_create();
    require(player, "offline decoder creates");
    require(mpv_set_option_string(player, "vo", "null") == 0 && mpv_set_option_string(player, "ao", "null") == 0
            && mpv_set_option_string(player, "audio", "no") == 0 && mpv_set_option_string(player, "hwdec", "no") == 0
            && mpv_set_option_string(player, "speed", "10") == 0 && mpv_initialize(player) == 0,
        "offline decoder initializes without network or GUI");
    const QByteArray bytes = url.toUtf8();
    const char *command[] = { "loadfile", bytes.constData(), nullptr };
    require(mpv_command(player, command) == 0, "offline file loads");
    bool loaded = false;
    bool decoded = false;
    bool ended = false;
    QElapsedTimer timer;
    timer.start();
    while (!ended && timer.elapsed() < 10000) {
        const mpv_event *event = mpv_wait_event(player, 0.1);
        if (event->event_id == MPV_EVENT_FILE_LOADED)
            loaded = true;
        if (event->event_id == MPV_EVENT_PLAYBACK_RESTART)
            decoded = true;
        if (event->event_id == MPV_EVENT_END_FILE) {
            const auto *end = static_cast<mpv_event_end_file *>(event->data);
            require(end && end->reason == MPV_END_FILE_REASON_EOF && end->error == 0,
                "offline media finishes without decode failure");
            ended = true;
        }
    }
    mpv_terminate_destroy(player);
    require(loaded && decoded && ended, "downloaded media actually decodes to EOF offline");
}
}

SPOOL_TEST_MAIN("download-manager")
{
    QCoreApplication app(argc, argv);
    std::setlocale(LC_NUMERIC, "C");
    using namespace Spool;
    QTemporaryDir root;
    qputenv("SPOOL_CREDENTIAL_STORE_DIR", root.filePath("credentials").toUtf8());
    const QByteArray media = readFile(QStringLiteral(TEST_SOURCE_DIR "/tests/media/fixtures/remux-h264.mp4"));
    require(media.size() > 1024, "real finite media fixture exists");
    QTcpServer server;
    QTcpServer foreign;
    require(server.listen(QHostAddress::LocalHost) && foreign.listen(QHostAddress::LocalHost), "media servers listen");
    int foreignRequests = 0;
    QObject::connect(&foreign, &QTcpServer::newConnection, &app, [&] {
        while (auto *socket = foreign.nextPendingConnection()) {
            ++foreignRequests;
            socket->disconnectFromHost();
            socket->deleteLater();
        }
    });
    int authenticated = 0;
    bool transcodeRequested = false;
    int createdSessions = 0;
    QObject::connect(&server, &QTcpServer::newConnection, &app, [&] {
        while (auto *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                if (socket->property("answered").toBool())
                    return;
                const QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                if (!request.contains("\r\n\r\n")) {
                    socket->setProperty("request", request);
                    return;
                }
                socket->setProperty("answered", true);
                require(request.contains("Authorization: Download account-secret"),
                    "download uses this plan's account headers");
                require(!request.toLower().contains("cookie:"), "download has no implicit cookies");
                ++authenticated;
                if (request.startsWith("GET /create-download-session")) {
                    ++createdSessions;
                    socket->write("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();
                    return;
                }
                if (request.startsWith("GET /playlist")) {
                    const QByteArray body("#EXTM3U\n#EXT-X-ENDLIST\n");
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                    return;
                }
                if (request.startsWith("GET /redirect")) {
                    socket->write(
                        "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:" + QByteArray::number(foreign.serverPort())
                        + "/steal.mp4\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();
                    return;
                }
                if (request.startsWith("GET /truncated")) {
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nContent-Length: "
                        + QByteArray::number(media.size()) + "\r\nConnection: close\r\n\r\n" + media.left(64));
                    socket->disconnectFromHost();
                    return;
                }
                transcodeRequested |= request.startsWith("GET /converted");
                // Unknown-length progressive output: real finite file bytes are paced
                // so progress and cancellation are observable before EOF.
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nConnection: close\r\n\r\n");
                auto *timer = new QTimer(socket);
                timer->setInterval(10);
                QObject::connect(timer, &QTimer::timeout, socket, [&, socket, timer] {
                    if (socket->state() != QAbstractSocket::ConnectedState) {
                        timer->stop();
                        return;
                    }
                    const int offset = socket->property("offset").toInt();
                    const int count = std::min(1024, static_cast<int>(media.size()) - offset);
                    if (count > 0) {
                        socket->write(media.constData() + offset, count);
                        socket->setProperty("offset", offset + count);
                    } else {
                        timer->stop();
                        socket->disconnectFromHost();
                    }
                });
                timer->start();
            });
        }
    });
    const QString origin = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    DatabaseManager database;
    require(database.initialize(root.filePath("cache.sqlite")), "download database initializes");
    auto package = ProviderFixture::package();
    auto manifest = QJsonDocument::fromJson(package.files["manifest.json"]).object();
    manifest.insert("capabilities", QJsonArray { "downloads", "downloadTranscode" });
    manifest.insert("origins", QJsonArray { origin });
    manifest.remove("extensions");
    package.files["manifest.json"] = QJsonDocument(manifest).toJson();
    package.manifest = *ProviderManifest::parse(package.files["manifest.json"]);
    package.files["logic/provider.mjs"] = R"JS(
export function createSource(config, sourceHost) {
    return {
        describe: function() { return {}; },
        details: function(args) { return {item:{id:args.itemId,title:'Offline ' + args.itemId,type:'Movie',runtimeTicks:'10000000'}}; },
        download: function(args, host) {
            if (args.itemId.indexOf('picker-') === 0) {
                if (!args.variantId)
                    return {pick:{kind:'download',variants:[{id:'edition',label:'Edition'}]}};
                return host.http(config.origin + '/create-download-session', {headers:{Authorization:'Download account-secret'}}).then(function() {
                    return {url:config.origin + '/original.mp4',container:'mp4',headers:{Authorization:'Download account-secret'},cleanup:{id:args.itemId}};
                });
            }
            const path = args.itemId === 'movie' ? (args.mode === 'transcoded' ? 'converted.mp4' : 'original.mp4') : args.itemId + '.mp4';
            return {url:config.origin + '/' + path,container:'mp4',headers:{Authorization:'Download account-secret'},cleanup:{id:args.itemId}};
        },
        downloadRelease: function(args) { sourceHost.emit('released', {id:args.cleanup.id}); return {}; }
    };
}
)JS";
    require(ProviderPackage::install(package, root.filePath("providers")).has_value(), "download source installs");
    ProviderRegistry registry(&database);
    registry.setInstallDirectory(root.filePath("providers"));
    registry.loadModules();
    SourceHub hub(&registry);
    QCoro::waitFor(registry.restore());
    const QString account = registry.finishSetup({},
        { { "module", "fixture.test" }, { "account", "download-account" }, { "label", "Downloads fixture" },
            { "configuration", QVariantMap { { "origin", origin } } } });
    waitUntil([&] { return registry.sourceRunning(account); }, "download account activates");
    int releases = 0;
    QObject::connect(
        &hub, &SourceHub::accountEvent, &app, [&](const QString&, const QString& type, const QVariantMap&) {
            if (type == "released")
                ++releases;
        });
    QList<QPointer<ProviderUiContext>> pickers;
    QObject::connect(&registry, &ProviderRegistry::componentRequested, &app,
        [&](QObject *context) { pickers.push_back(qobject_cast<ProviderUiContext *>(context)); });
    const QString movie = hub.scoped(account, "movie");
    QString completedId;
    QString completedPath;
    {
        DownloadManager downloads(&hub, root.filePath("ledger"));
        downloads.setDestination(QUrl::fromLocalFile(root.filePath("media")));
        const QString pickerCancelledItem = hub.scoped(account, "picker-cancelled");
        const QString pickerOtherItem = hub.scoped(account, "picker-other");
        downloads.start(pickerCancelledItem, hub.downloadOptions(pickerCancelledItem).front().toMap());
        waitUntil([&] { return pickers.size() == 1; }, "download opens its provider picker");
        const QPointer<ProviderUiContext> cancelledPicker = pickers.front();
        downloads.start(pickerOtherItem, hub.downloadOptions(pickerOtherItem).front().toMap());
        waitUntil([&] { return pickers.size() == 2; }, "another scoped download opens independently");
        const QPointer<ProviderUiContext> otherPicker = pickers.back();
        bool ordinarySubmitted = false;
        registry.pick(account, { { "kind", "ordinary-playback" } }).then([&](QVariantMap result) {
            ordinarySubmitted = result.value("variantId").toString() == "edition";
        });
        waitUntil([&] { return pickers.size() == 3; }, "ordinary playback picker opens without download scope");
        const QPointer<ProviderUiContext> ordinaryPicker = pickers.back();
        downloads.cancel(downloads.statusFor(pickerCancelledItem).value("id").toString());
        if (cancelledPicker)
            cancelledPicker->complete({ { "variantId", "edition" } });
        // Drain the real worker/network result of a stale picker submission.
        QEventLoop staleSubmission;
        QTimer::singleShot(500, &staleSubmission, &QEventLoop::quit);
        staleSubmission.exec();
        require(createdSessions == 0, "a cancelled picker cannot create a server download session");
        require(!cancelledPicker || cancelledPicker->closed(), "cancel retires its pending picker");
        require(otherPicker && !otherPicker->closed(), "cancelling one download preserves another picker");
        require(
            ordinaryPicker && !ordinaryPicker->closed(), "download cancellation leaves ordinary playback picker open");
        ordinaryPicker->complete({ { "variantId", "edition" } });
        waitUntil([&] { return ordinarySubmitted; }, "ordinary playback submission still completes");
        otherPicker->complete({ { "variantId", "edition" } });
        waitUntil([&] { return downloads.statusFor(pickerOtherItem).value("state").toString() == "complete"; },
            "independent picker still starts and completes its transfer");
        require(createdSessions == 1, "only the submitted live download creates a server session");
        downloads.remove(downloads.statusFor(pickerOtherItem).value("id").toString());
        const auto options = hub.downloadOptions(movie);
        require(options.size() > 1, "server quality offered for this account");
        const auto quality = options[1].toMap();
        downloads.start(movie, quality);
        waitUntil([&] { return downloads.statusFor(movie).value("received").toLongLong() > 0; },
            "native streaming reports progress before completion");
        require(downloads.statusFor(movie).value("state").toString() == "downloading",
            "progress is visible while finite output streams");
        waitUntil([&] { return downloads.statusFor(movie).value("state").toString() == "complete"; },
            "finite transcode download completes");
        completedId = downloads.statusFor(movie).value("id").toString();
        completedPath = downloads.libraryFiles().front().toMap().value("path").toString();
        require(readFile(completedPath) == media && transcodeRequested,
            "server output is stored exactly, not a playlist or client encode");
        require(downloads.offlineItemId(completedId).startsWith(hub.scoped("spool-downloads", "")),
            "offline id crosses the existing scoped provider seam");
        const QString cancelItem = hub.scoped(account, "cancel");
        downloads.start(cancelItem, hub.downloadOptions(cancelItem).front().toMap());
        waitUntil([&] { return downloads.statusFor(cancelItem).value("received").toLongLong() > 0; },
            "cancel transfer starts");
        const QString cancelledId = downloads.statusFor(cancelItem).value("id").toString();
        downloads.cancel(cancelledId);
        require(downloads.statusFor(cancelItem).value("state").toString() == "cancelled",
            "cancel is explicit and terminal");
        require(QDir(root.filePath("media")).entryList(QDir::Files).size() == 1,
            "cancel deletes partial output without damaging completed files");
        for (const QString& failure :
            { QStringLiteral("playlist"), QStringLiteral("redirect"), QStringLiteral("truncated") }) {
            const QString item = hub.scoped(account, failure);
            downloads.start(item, hub.downloadOptions(item).front().toMap());
            waitUntil([&] { return downloads.statusFor(item).value("state").toString() == "failed"; },
                "invalid download fails visibly");
            require(!downloads.statusFor(item).value("error").toString().isEmpty(),
                "failed transfer exposes an actionable status");
            require(QDir(root.filePath("media")).entryList(QDir::Files).size() == 1,
                "failure never publishes a partial or manifest file");
        }
        require(foreignRequests == 0, "redirect never leaks download authentication to a foreign origin");
        waitUntil(
            [&] { return releases >= 6; }, "every negotiated session releases after success cancellation and failure");
        const QString removedItem = hub.scoped(account, "picker-account-removed");
        downloads.start(removedItem, hub.downloadOptions(removedItem).front().toMap());
        waitUntil([&] { return pickers.size() == 4; }, "download picker waits when account is removed");
        const QPointer<ProviderUiContext> removedPicker = pickers.back();
        registry.removeAccount(account);
        // Account removal first awaits best-effort server sign-out.
        waitUntil([&] { return !registry.sourceRunning(account) && (!removedPicker || removedPicker->closed()); },
            "completed account removal retires pending download picker");
        if (removedPicker)
            removedPicker->complete({ { "variantId", "edition" } });
        QEventLoop removedSubmission;
        QTimer::singleShot(500, &removedSubmission, &QEventLoop::quit);
        removedSubmission.exec();
        require(createdSessions == 1, "removed account cannot create a download session from a stale picker");
        require(downloads.statusFor(removedItem).value("state").toString() == "failed",
            "account removal exposes download failure rather than retaining preparation");
    }
    require(authenticated >= 5, "actual authenticated streaming requests exercised");
    server.close();
    {
        DownloadManager restarted(&hub, root.filePath("ledger"));
        require(restarted.statusFor(movie).value("state").toString() == "complete",
            "completed download persists across restart without the account");
        LocalProvider local("spool-downloads", {}, nullptr, restarted.libraryFiles(), root.filePath("offline-state"));
        local.scan();
        hub.addSource(&local);
        const QString offline = restarted.offlineItemId(completedId);
        const MovieItem item = QCoro::waitFor(hub.fetchItemDetails(offline));
        require(item.title == "Offline movie", "offline metadata preserves remote title without a remote request");
        const auto session = QCoro::waitFor(hub.playback()->resolvePlayback(item, false));
        require(QUrl(session.url).isLocalFile() && hub.playback()->mediaRequestHeaders().isEmpty(),
            "offline playback uses only the local file with no stale account headers");
        decodeOffline(session.url);
        QCoro::waitFor(hub.setItemPlaybackPosition(offline, 5000000));
        hub.removeSource("spool-downloads");
        LocalProvider second("spool-downloads", {}, nullptr, restarted.libraryFiles(), root.filePath("offline-state"));
        second.scan();
        const auto details = QCoro::waitFor(second.fetchItemDetails(SourceHub::rawId(offline)));
        require(details.resumeTicks == 5000000, "offline progress survives a local provider restart");
        restarted.remove(completedId);
        require(!QFileInfo::exists(completedPath) && restarted.libraryFiles().isEmpty(),
            "remove deletes downloaded media and offline inventory");
    }
    return 0;
}
