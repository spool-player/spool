#include "ProviderFixture.h"
#include "TestMain.h"
#include "app/ArtworkService.h"
#include "cache/DatabaseManager.h"
#include "provider/ProviderRegistry.h"
#include "provider/SourceHub.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QQuickImageResponse>
#include <QQuickTextureFactory>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>

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
    QElapsedTimer deadline;
    deadline.start();
    while (!condition() && deadline.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(condition(), message);
}
QByteArray png(QColor color)
{
    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"), "fixture image encodes");
    return bytes;
}
}

SPOOL_TEST_MAIN("artwork-authentication")
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    using namespace Spool;
    QTemporaryDir directory;
    qputenv("SPOOL_CREDENTIAL_STORE_DIR", directory.filePath("credentials").toUtf8());
    const QByteArray aliceImage = png(Qt::green);
    const QByteArray bobImage = png(Qt::red);
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost), "protected image server listens");
    QTcpServer foreignServer;
    require(foreignServer.listen(QHostAddress::LocalHost), "foreign-origin sentinel listens");
    int foreignRequests = 0;
    QObject::connect(&foreignServer, &QTcpServer::newConnection, &foreignServer, [&] {
        while (auto *socket = foreignServer.nextPendingConnection()) {
            ++foreignRequests;
            socket->write("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    int requests = 0;
    int unauthorized = 0;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (auto *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                if (socket->property("answered").toBool())
                    return;
                const QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                if (!request.contains("\r\n\r\n")) {
                    socket->setProperty("request", request);
                    return;
                }
                socket->setProperty("answered", true);
                ++requests;
                if (request.startsWith("GET /redirect ")) {
                    socket->write("HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:"
                        + QByteArray::number(foreignServer.serverPort())
                        + "/steal\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();
                    return;
                }
                const QByteArray normalized = request.toLower();
                const bool alice = normalized.contains("authorization: mediabrowser token=\"alice-secret\"");
                const bool bob = normalized.contains("authorization: mediabrowser token=\"bob-secret\"");
                const QByteArray bytes = alice ? aliceImage : bob ? bobImage : QByteArray();
                if (bytes.isEmpty())
                    ++unauthorized;
                socket->write(QByteArray(bytes.isEmpty() ? "HTTP/1.1 401 Unauthorized\r\n" : "HTTP/1.1 200 OK\r\n")
                    + "Content-Type: image/png\r\nCache-Control: public, max-age=86400\r\nContent-Length: "
                    + QByteArray::number(bytes.size()) + "\r\nConnection: close\r\n\r\n" + bytes);
                socket->disconnectFromHost();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    const QString origin = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    DatabaseManager database;
    require(database.initialize(directory.filePath("cache.sqlite")), "database opens");
    auto package = ProviderFixture::package();
    auto manifest = QJsonDocument::fromJson(package.files.value("manifest.json")).object();
    manifest.insert("extensions", QJsonObject { { "spool.remote-targets", 1 } });
    manifest.insert("origins", QJsonArray { origin });
    package.files["manifest.json"] = QJsonDocument(manifest).toJson();
    package.manifest = *ProviderManifest::parse(package.files.value("manifest.json"));
    package.files["logic/provider.mjs"] = R"JS(
export function createSource(config) {
    return {
        describe: function() { return {extensions: {'spool.remote-targets': 1},
            trickplay: config.origin + '/sheet/{index}.png'}; },
        remoteState: function() { return {state:'paused', commands:['seek'], item:{id:'movie', title:'Movie'},
            preview:{width:16, height:16, columns:1, rows:1, count:1, intervalMs:1000,
                urlTemplate:config.origin + '/remote/{index}.png',
                headers:{Authorization:'MediaBrowser Token="' + config.token + '"'}}}; },
        resolve: function(args) { return {url: config.origin + '/video', variantId: 'edition',
            headers: {Authorization: 'MediaBrowser Token="' + config.token + '"'},
            streams: [{index: 0, type: 'Video'},
                {index: 10000, type: 'Subtitle', external: true, url: config.origin + '/subs/1.srt'},
                {index: 10001, type: 'Subtitle', external: true, url: 'https://elsewhere.invalid/2.srt'},
                {index: 2, type: 'Subtitle'}, {index: 10002, type: 'Subtitle', external: true}],
            trickplay: {width:16, height:16, columns:1, rows:1, count:1, intervalMs:1000}}; }
    };
}
)JS";
    require(ProviderPackage::install(package, directory.filePath("providers")).has_value(), "fixture installs");
    ProviderRegistry registry(&database);
    registry.setInstallDirectory(directory.filePath("providers"));
    registry.loadModules();
    SourceHub hub(&registry);
    QCoro::waitFor(registry.restore());
    const auto add = [&](const QString& token) {
        const QString id = registry.finishSetup({},
            { { "module", "fixture.test" }, { "account", token }, { "label", "Fixture" },
                { "configuration", QVariantMap { { "origin", origin }, { "token", token } } } });
        waitUntil([&] { return registry.sourceRunning(id); }, "source activates");
        MovieItem item;
        item.id = hub.scoped(id, "movie");
        const PlaybackSession session = QCoro::waitFor(hub.playback()->resolvePlayback(item, false));
        QList<int> order;
        for (const MediaStreamInfo& stream : session.mediaStreams)
            order.append(stream.index);
        require(
            order == QList<int> { 0, 2, 10000 } && session.mediaStreams.last().deliveryUrl == origin + "/subs/1.srt",
            "subtitle files follow the file's own tracks, and only those on the stream's origin are kept");
        return std::pair(id, hub.playback()->trickplayTileUrl(item.id, 16, 0));
    };
    const auto alice = add("alice-secret");
    const auto bob = add("bob-secret");
    require(!alice.second.contains("alice-secret") && !bob.second.contains("bob-secret"),
        "preview URLs do not expose credentials to QML");
    ArtworkService artwork(directory.filePath("artwork"), 1024 * 1024, 1024 * 1024, 1, nullptr);
    artwork.setSource(&hub);
    const auto fetch = [&](const QString& url, bool success) {
        std::unique_ptr<QQuickImageResponse> response(
            artwork.requestImageResponse(QString::fromLatin1(QUrl::toPercentEncoding(url)), {}));
        bool finished = false;
        QObject::connect(response.get(), &QQuickImageResponse::finished, &app, [&] { finished = true; });
        waitUntil([&] { return finished; }, "preview completes");
        if (response->errorString().isEmpty() != success)
            std::cerr << "preview request " << QUrl(url).scheme().toStdString() << " expected=" << success
                      << " error=" << response->errorString().toStdString() << " requests=" << requests
                      << " unauthorized=" << unauthorized << '\n';
        require(response->errorString().isEmpty() == success, "preview authentication result is explicit");
        if (!success)
            return QImage();
        std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
        require(bool(texture), "authenticated preview creates a texture");
        return texture->image();
    };
    require(fetch(alice.second, true).pixelColor(8, 8) == QColor(Qt::green),
        "Alice's preview authenticates after Bob becomes active");
    require(fetch(bob.second, true).pixelColor(8, 8) == QColor(Qt::red),
        "same-URL caches cannot expose Alice's image to Bob");
    require(fetch(alice.second, true).pixelColor(8, 8) == QColor(Qt::green),
        "cached previews retain their account partition");
    require(requests == 2 && unauthorized == 0, "valid previews carry headers and reuse their own memory cache");
    {
        std::unique_ptr<QQuickImageResponse> cancelled(
            artwork.requestImageResponse(QString::fromLatin1(QUrl::toPercentEncoding(alice.second)), {}));
        bool completed = false;
        QObject::connect(cancelled.get(), &QQuickImageResponse::finished, &app, [&] { completed = true; });
        cancelled->cancel();
        waitUntil([&] { return completed; }, "cancelling a queued preview still finishes the response");
        require(!cancelled->errorString().isEmpty() && requests == 2,
            "cancelled previews cannot start an authenticated request or masquerade as a decoded image");
    }
    const auto remote = QCoro::waitFor(hub.remoteState(hub.scoped(alice.first, "target"), false, "preview"));
    const auto descriptor = remote.value("preview").toMap();
    require(!descriptor.contains("headers"), "remote preview credentials never reach QML");
    QString remoteUrl = descriptor.value("urlTemplate").toString();
    remoteUrl.replace("{index}", "0");
    require(fetch(remoteUrl, true).pixelColor(8, 8) == QColor(Qt::green),
        "remote previews authenticate as their owner without selecting its local playback");
    artwork.releaseMemory(false);
    require(fetch(bob.second, true).pixelColor(8, 8) == QColor(Qt::red),
        "protected previews bypass the URL-only disk cache after memory eviction");
    const auto routed = [&](const QString& networkUrl) {
        QUrl resource(alice.second);
        resource.setPath('/'
            + QString::fromLatin1(
                networkUrl.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
        return resource.toString();
    };
    fetch(routed(QStringLiteral("http://127.0.0.1:%1/steal").arg(foreignServer.serverPort())), false);
    fetch(routed(origin + "/redirect"), false);
    registry.removeAccount(alice.first);
    waitUntil(
        [&] { return hub.source(alice.first) == nullptr; }, "account removal completes before reusing its preview URL");
    fetch(alice.second, false);
    fetch(remoteUrl, false);
    require(requests == 5 && foreignRequests == 0,
        "foreign origins, redirects and removed accounts cannot receive credentials or cached previews");
    return 0;
}
