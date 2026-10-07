#include "ProviderFixture.h"
#include "TestMain.h"
#include "app/ArtworkService.h"
#include "app/TrickplayService.h"
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
#if !defined(Q_OS_ANDROID) && !defined(SPOOL_APPLE_MOBILE)
    qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
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
                if (request.startsWith("GET /redirect")) {
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
    manifest.insert("capabilities", QJsonArray { "remoteTargets" });
    manifest.insert("origins", QJsonArray { origin });
    package.files["manifest.json"] = QJsonDocument(manifest).toJson();
    package.manifest = *ProviderManifest::parse(package.files.value("manifest.json"));
    package.files["logic/provider.mjs"] = R"JS(
export function createSource(config) {
    let remoteRevision = 0;
    return {
        describe: function() { return {capabilities: {remoteTargets: true}}; },
        remoteState: function() {
            const revision = remoteRevision++;
            const token = revision < 2 ? config.token : 'bob-secret';
            return {state:'paused', commands:['seek'], item:{id:'movie', title:'Movie'},
                preview:{width:16, height:16, columns:1, rows:1, count:1, intervalMs:1000,
                    urlTemplate:config.origin + (revision === 0 ? '/remote/{index}.png' : '/remote-alt/{index}.png'),
                    headers:{Authorization:'MediaBrowser Token="' + token + '"'}}};
        },
        resolve: function(args) { return {url: config.origin + '/video', variantId: 'edition',
            headers: {Authorization: 'MediaBrowser Token="' + config.token + '"'},
            streams: [{index: 0, type: 'Video'},
                {index: 10000, type: 'Subtitle', external: true, url: config.origin + '/subs/1.srt'},
                {index: 10001, type: 'Subtitle', external: true, url: 'https://elsewhere.invalid/2.srt'},
                {index: 2, type: 'Subtitle'}, {index: 10002, type: 'Subtitle', external: true}],
            trickplay: {width:16, height:16, columns:1, rows:1, count:1, intervalMs:1000,
                urlTemplate: config.previewUrl || config.origin + '/sheet/{index}.png'}}; }
    };
}
)JS";
    require(ProviderPackage::install(package, directory.filePath("providers")).has_value(), "fixture installs");
    ProviderRegistry registry(&database);
    registry.setInstallDirectory(directory.filePath("providers"));
    registry.loadModules();
    SourceHub hub(&registry);
    // A digit-only scope is a valid UUID prefix, but a bare URL hostname is
    // interpreted by Qt as an IPv4 address. Exercise that boundary every run.
    QCoro::waitFor(database.saveSettings(
        { { "providers/accounts/2", QStringLiteral(R"JSON({"accounts":[{"id":"01234567-89ab-4cde-8fab-0123456789ab",
        "module":"fixture.test","key":"alice-secret","label":"Fixture","enabled":false}]})JSON") } }));
    QCoro::waitFor(registry.restore());
    const auto add = [&](const QString& token, const QString& previewUrl = QString()) {
        const QString id = registry.finishSetup({},
            { { "module", "fixture.test" }, { "account", token }, { "label", "Fixture" },
                { "configuration",
                    QVariantMap { { "origin", origin }, { "token", token }, { "previewUrl", previewUrl } } } });
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
        QString url = session.trickplay.urlTemplate;
        url.replace("{index}", "0");
        return std::pair(id, url);
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
    TrickplayService nativeAlice(&hub, nullptr), nativeBob(&hub, nullptr);
    const auto nativeInfo = [](const QString& scoped) {
        TrickplayInfo info;
        info.width = info.height = 16;
        info.tileWidth = info.tileHeight = info.thumbnailCount = 1;
        info.intervalMs = 1000;
        info.urlTemplate = scoped;
        return info;
    };
    nativeAlice.setSession(nativeInfo(alice.second), 0);
    nativeBob.setSession(nativeInfo(bob.second), 0);
    const auto nativeFetch = [&](TrickplayService& service, bool success) {
        const QString id = QUrl(service.frame(0).value("url").toString()).path().mid(1);
        std::unique_ptr<QQuickImageResponse> response(service.requestImageResponse(id));
        bool finished = false;
        QObject::connect(response.get(), &QQuickImageResponse::finished, &app, [&] { finished = true; });
        waitUntil([&] { return finished; }, "native authenticated preview completes");
        require(response->errorString().isEmpty() == success, "native preview reports account authorization");
        if (!success)
            return QImage();
        std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
        require(bool(texture), "native protected preview produces real frame pixels");
        return texture->image();
    };
    require(nativeFetch(nativeAlice, true).pixelColor(8, 8) == QColor(Qt::green)
            && nativeFetch(nativeBob, true).pixelColor(8, 8) == QColor(Qt::red)
            && nativeFetch(nativeAlice, true).pixelColor(8, 8) == QColor(Qt::green),
        "dedicated native frames keep same-URL credentials and decoded caches account-isolated");
    TrickplayService nativeRemote(&hub, nullptr);
    nativeRemote.setSession(nativeInfo(remoteUrl), 0);
    require(nativeFetch(nativeRemote, true).pixelColor(8, 8) == QColor(Qt::green),
        "initial remote resource produces its authenticated native frame");
    const auto changedResource = QCoro::waitFor(hub.remoteState(hub.scoped(alice.first, "target"), false, "preview"));
    QString changedUrl = changedResource.value("preview").toMap().value("urlTemplate").toString();
    changedUrl.replace("{index}", "0");
    require(changedResource.value("item") == remote.value("item") && changedUrl != remoteUrl,
        "same-item remote edition changes invalidate the scoped resource identity");
    fetch(remoteUrl, false);
    nativeFetch(nativeRemote, false);
    nativeRemote.setSession(nativeInfo(changedUrl), 0);
    require(nativeFetch(nativeRemote, true).pixelColor(8, 8) == QColor(Qt::green),
        "replacement remote resource loads rather than reusing its retired native frame");
    const auto changedAuth = QCoro::waitFor(hub.remoteState(hub.scoped(alice.first, "target"), false, "preview"));
    QString credentialUrl = changedAuth.value("preview").toMap().value("urlTemplate").toString();
    credentialUrl.replace("{index}", "0");
    require(credentialUrl != changedUrl && !credentialUrl.contains("bob-secret"),
        "header-only changes revise the opaque resource without exposing credentials");
    fetch(changedUrl, false);
    nativeFetch(nativeRemote, false);
    nativeRemote.setSession(nativeInfo(credentialUrl), 0);
    require(nativeFetch(nativeRemote, true).pixelColor(8, 8) == QColor(Qt::red),
        "same-URL remote authentication changes cannot retain previous credential-colored pixels");
    const auto unchanged = QCoro::waitFor(hub.remoteState(hub.scoped(alice.first, "target"), false, "preview"));
    require(unchanged.value("preview") == changedAuth.value("preview")
            && nativeFetch(nativeRemote, true).pixelColor(8, 8) == QColor(Qt::red),
        "unchanged remote resources preserve scoped identity and decoded cache reuse");
    remoteUrl = credentialUrl;
    hub.setVideoPreviewsEnabled(false);
    require(hub.resolveImage(QUrl(alice.second)).url.isEmpty() && hub.resolveImage(QUrl(remoteUrl)).url.isEmpty(),
        "global preview opt-out revokes local and remote protected resources already issued");
    const auto disabledRemote = QCoro::waitFor(hub.remoteState(hub.scoped(alice.first, "target"), false, "preview"));
    require(!disabledRemote.contains("preview"), "remote metadata cannot expose previews while disabled");
    hub.setVideoPreviewsEnabled(true);
    QCoro::waitFor(hub.remoteState(hub.scoped(alice.first, "target"), false, "preview"));
    const auto foreign
        = add("foreign-secret", QStringLiteral("http://127.0.0.1:%1/steal/{index}").arg(foreignServer.serverPort()));
    require(foreign.second.isEmpty(), "a provider cannot describe a preview on a foreign media origin");
    fetch(foreign.second, false);
    const auto redirected = add("redirect-secret", origin + "/redirect?index={index}");
    fetch(redirected.second, false);
    registry.removeAccount(alice.first);
    waitUntil(
        [&] { return hub.source(alice.first) == nullptr; }, "account removal completes before reusing its preview URL");
    fetch(alice.second, false);
    fetch(remoteUrl, false);
    nativeFetch(nativeAlice, false);
    nativeFetch(nativeRemote, false);
    require(requests == 10 && foreignRequests == 0,
        "foreign origins, redirects and removed accounts cannot receive credentials or cached previews");
    return 0;
}
