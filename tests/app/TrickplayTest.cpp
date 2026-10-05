#include "TestMain.h"
#include "app/TrickplayDecoder.h"
#include "app/TrickplayService.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImageReader>
#include <QPainter>
#include <QPointer>
#include <QQuickTextureFactory>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>

#include <cstdio>
#if defined(SPOOL_QT_BUNDLED_JPEG)
#include <QtJpeg/jpeglib.h>
#else
#include <jpeglib.h>
#endif
using namespace Spool;
namespace {
void require(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
template <typename Predicate> void waitUntil(Predicate predicate, const char *message = "native preview did not finish")
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), message);
}
QByteArray jpeg(const QImage& image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    require(image.save(&buffer, "JPEG", 95), "JPEG fixture encoder available");
    return bytes;
}
QByteArray jpegSampling(const QImage& image, int horizontal, int vertical)
{
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    jpeg_compress_struct codec {};
    jpeg_error_mgr error {};
    codec.err = jpeg_std_error(&error);
    jpeg_create_compress(&codec);
    unsigned char *data = nullptr;
    unsigned long size = 0;
    jpeg_mem_dest(&codec, &data, &size);
    codec.image_width = rgb.width();
    codec.image_height = rgb.height();
    codec.input_components = 3;
    codec.in_color_space = JCS_RGB;
    jpeg_set_defaults(&codec);
    codec.comp_info[0].h_samp_factor = horizontal;
    codec.comp_info[0].v_samp_factor = vertical;
    jpeg_set_quality(&codec, 95, TRUE);
    jpeg_start_compress(&codec, TRUE);
    while (codec.next_scanline < codec.image_height) {
        auto *row = const_cast<JSAMPLE *>(rgb.constScanLine(int(codec.next_scanline)));
        jpeg_write_scanlines(&codec, &row, 1);
    }
    jpeg_finish_compress(&codec);
    const QByteArray bytes(reinterpret_cast<const char *>(data), qsizetype(size));
    std::free(data);
    jpeg_destroy_compress(&codec);
    return bytes;
}
void word(QByteArray& bytes, qsizetype offset, quint32 value)
{
    qToLittleEndian(value, reinterpret_cast<uchar *>(bytes.data() + offset));
}
QByteArray bif(const QByteArray& first, const QByteArray& second)
{
    QByteArray bytes(88, '\0');
    bytes.replace(0, 8, QByteArray::fromHex("894249460d0a1a0a"));
    word(bytes, 12, 2);
    word(bytes, 16, 500);
    word(bytes, 64, 3);
    word(bytes, 68, 88);
    word(bytes, 72, 9);
    word(bytes, 76, 88 + first.size());
    word(bytes, 80, 0xffffffffu);
    word(bytes, 84, 88 + first.size() + second.size());
    return bytes + first + second;
}
void color(const QImage& image, QColor expected)
{
    require(!image.isNull(), "decoded real preview is not null");
    const QColor actual = image.pixelColor(image.width() / 2, image.height() / 2);
    require(std::abs(actual.red() - expected.red()) <= 5 && std::abs(actual.green() - expected.green()) <= 5
            && std::abs(actual.blue() - expected.blue()) <= 5,
        "preview selected the wrong frame color");
}
QString fixture(QTemporaryDir& directory, const char *name, const QByteArray& bytes)
{
    const QString path = directory.filePath(QLatin1String(name));
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "preview fixture writes");
    return QUrl::fromLocalFile(path).toString();
}
class FixtureSource final : public ArtworkSource {
    QString imageUrl(const ImageRequest&) const override
    {
        return {};
    }
};
std::unique_ptr<QQuickTextureFactory> textureForSeconds(TrickplayService& service, double seconds, bool success = true)
{
    const auto descriptor = service.frame(seconds);
    require(descriptor.value("available").toBool(), "native frame descriptor available");
    const QString id = QUrl(descriptor.value("url").toString()).path().mid(1);
    std::unique_ptr<QQuickImageResponse> response(service.requestImageResponse(id));
    bool finished = false;
    QObject::connect(response.get(), &QQuickImageResponse::finished, [&] { finished = true; });
    waitUntil([&] { return finished; });
    require(response->errorString().isEmpty() == success, "native frame completion result explicit");
    if (!success)
        return {};
    std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
    require(bool(texture), "native output creates a real texture");
    return texture;
}
QImage frame(TrickplayService& service, double seconds, bool success = true, QImage *residentTexture = nullptr)
{
    auto texture = textureForSeconds(service, seconds, success);
    if (!texture)
        return {};
    const auto descriptor = service.frame(seconds);
    const QImage image = texture->image();
    require(image.width() == descriptor.value("sheetWidth").toInt()
            && image.height() == descriptor.value("sheetHeight").toInt(),
        "texture dimensions match descriptor");
    if (residentTexture)
        *residentTexture = image;
    const QRect crop(-descriptor.value("offsetX").toInt(), -descriptor.value("offsetY").toInt(),
        descriptor.value("width").toInt(), descriptor.value("height").toInt());
    require(QRect(QPoint(), image.size()).contains(crop), "descriptor crop stays within delivered texture");
    return image.copy(crop);
}
}

SPOOL_TEST_MAIN("trickplay")
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication application(argc, argv);
    QImage odd(33, 19, QImage::Format_RGB32);
    odd.fill(Qt::cyan);
    const QByteArray odd420 = jpegSampling(odd, 2, 2);
    for (bool accurate : { false, true }) {
        QString decodeError;
        const auto raw = decodeTrickplayTexture(odd420, {}, odd.size(), false, accurate, &decodeError);
        require(raw && raw->planar() && raw->size == odd.size() && decodeError.isEmpty(),
            "both JPEG transforms preserve odd visible 420 dimensions without RGB intermediate");
        for (int i = 0; i < 3; ++i) {
            const int width = i == 0 ? odd.width() : (odd.width() + 1) / 2;
            const int height = i == 0 ? odd.height() : (odd.height() + 1) / 2;
            require(
                raw->strides[i] >= width && raw->planes[i].size() >= qsizetype(height - 1) * raw->strides[i] + width,
                "raw plane backing covers every visible row including partial MCU");
        }
        const QImage reconstructed = raw->image();
        color(reconstructed, Qt::cyan);
        require(reconstructed.pixelColor(0, 0).green() > 250 && reconstructed.pixelColor(32, 18).blue() > 250,
            "partial MCU edges reconstruct real pixels");
    }
    for (const auto factors : { QSize(2, 1), QSize(1, 1) }) {
        QString decodeError;
        const auto rgb = decodeTrickplayTexture(
            jpegSampling(odd, factors.width(), factors.height()), {}, odd.size(), false, false, &decodeError);
        require(rgb && !rgb->planar() && decodeError.isEmpty(), "422 and 444 JPEGs retain a valid RGB fallback");
        color(rgb->image(), Qt::cyan);
    }
    QImage gray(33, 19, QImage::Format_Grayscale8);
    gray.fill(96);
    QString grayError;
    const auto grayTexture = decodeTrickplayTexture(jpeg(gray), {}, gray.size(), false, false, &grayError);
    require(grayTexture && !grayTexture->planar() && grayError.isEmpty(),
        "grayscale JPEG uses the RGB fallback rather than interpreting missing chroma planes");
    color(grayTexture->image(), QColor(96, 96, 96));
    QTemporaryDir directory;
    QImage green(32, 18, QImage::Format_RGB32), blue(48, 27, QImage::Format_RGB32);
    green.fill(Qt::green);
    blue.fill(Qt::blue);
    const QByteArray sequence = bif(jpeg(green), jpeg(blue));
    BifSequence parsed;
    QString error;
    require(parsed.parse(sequence, &error), "valid BIF parses");
    require(parsed.frameAt(-5) == 0 && parsed.frameAt(4.499) == 0 && parsed.frameAt(4.5) == 1
            && parsed.frameAt(100000) == 1,
        "BIF timestamps and multiplier, not uniform metadata, select frames");
    require(parsed.frameAt(std::numeric_limits<double>::quiet_NaN()) == -1
            && parsed.frameAt(std::numeric_limits<double>::infinity()) == -1,
        "nonfinite BIF times rejected");
    QByteArray defaultMultiplier = sequence;
    word(defaultMultiplier, 16, 0);
    require(parsed.parse(defaultMultiplier, &error) && parsed.frameAt(8.999) == 0 && parsed.frameAt(9) == 1,
        "zero BIF multiplier means 1000ms");
    QList<QByteArray> malformed;
    malformed << sequence.first(63) << sequence.first(87);
    for (const auto mutation :
        QList<QPair<qsizetype, quint32>> { { 0, 0 }, { 8, 1 }, { 12, 0 }, { 12, 0xffffffffu }, { 68, 87 }, { 76, 88 },
            { 72, 2 }, { 72, 3 }, { 80, 0 }, { 84, quint32(sequence.size() + 1) }, { 84, 1 } }) {
        QByteArray bytes = sequence;
        word(bytes, mutation.first, mutation.second);
        malformed.append(bytes);
    }
    for (const auto& bytes : malformed) {
        require(!parsed.parse(bytes, &error) && !error.isEmpty() && parsed.frameAt(0) == -1,
            "malformed BIF boundaries reject without retaining prior sequence");
    }
    FixtureSource source;
    TrickplayService service(&source, nullptr);
    TrickplayInfo info;
    info.format = "bif";
    info.url = fixture(directory, "index.bif", sequence);
    service.setSession(info, 4.5);
    require(!service.frame(4.5).value("available").toBool(), "BIF stays unavailable until index and dimensions load");
    waitUntil([&] { return service.frame(4.5).value("available").toBool(); });
    color(frame(service, 4.5), Qt::blue);
    color(frame(service, 4.499), Qt::green);
    require(service.frame(4.5).value("url") == service.frame(8.7).value("url"), "BIF URL identifies discrete frame");
    const QString stale = QUrl(service.frame(4.5).value("url").toString()).path().mid(1);
    service.clear();
    std::unique_ptr<QQuickImageResponse> staleResponse(service.requestImageResponse(stale));
    bool staleFinished = false;
    QObject::connect(staleResponse.get(), &QQuickImageResponse::finished, [&] { staleFinished = true; });
    waitUntil([&] { return staleFinished; });
    require(!staleResponse->errorString().isEmpty(), "old session cannot deliver a cached BIF image");

    QImage sheet(3200, 1800, QImage::Format_RGB32);
    QPainter painter(&sheet);
    for (int tile = 0; tile < 100; ++tile)
        painter.fillRect((tile % 10) * 320, (tile / 10) * 180, 320, 180, tile % 2 ? Qt::blue : Qt::green);
    painter.end();
    const QByteArray encoded = jpeg(sheet);
    QElapsedTimer baselineTimer;
    baselineTimer.start();
    const QImage oldSheet = QImage::fromData(encoded, "JPEG");
    const double baselineMs = baselineTimer.nsecsElapsed() / 1000000.0;
    require(oldSheet.size() == sheet.size(), "baseline decodes actual representative full sheet");
    info = {};
    info.width = 320;
    info.height = 180;
    info.tileWidth = 10;
    info.tileHeight = 10;
    info.thumbnailCount = 100;
    info.intervalMs = 1000;
    info.urlTemplate = fixture(directory, "sheet-0.jpg", encoded);
    info.urlTemplate.replace("sheet-0", "sheet-{index}");
    QElapsedTimer cold;
    cold.start();
    service.setSession(info, 0);
    QImage firstTexture;
    const QImage first = frame(service, 0, true, &firstTexture);
    const double coldMs = cold.nsecsElapsed() / 1000000.0;
    color(first, Qt::green);
    require(service.frame(0).value("url") == service.frame(99).value("url"),
        "distinct resident-sheet tiles share one texture URL");
    require(service.frame(23).value("offsetX").toInt() == -960 && service.frame(23).value("offsetY").toInt() == -360,
        "resident-sheet descriptor locates both crop axes");
    require(firstTexture.size() == sheet.size(), "resident sheet is delivered without per-tile copies");
    auto firstFactory = textureForSeconds(service, 0);
    auto *firstBacking = dynamic_cast<TrickplayTextureFactory *>(firstFactory.get());
    require(firstBacking != nullptr, "resident output exposes the decoded payload used by the renderer");
    require(!service.frame(100).value("available").toBool()
            && !service.frame(std::numeric_limits<double>::infinity()).value("available").toBool(),
        "sprite end and nonfinite positions unavailable");
    color(frame(service, -1), Qt::green);
    int warmChanges = 0;
    const auto warmConnection = QObject::connect(&service, &TrickplayService::changed, [&] { ++warmChanges; });
    QElapsedTimer warm;
    warm.start();
    for (int tile = 1; tile <= 24; ++tile) {
        auto texture = textureForSeconds(service, tile);
        auto *backing = dynamic_cast<TrickplayTextureFactory *>(texture.get());
        require(backing && backing->texture == firstBacking->texture, "warm tiles reuse immutable resident backing");
        const auto selected = service.frame(tile);
        color(firstTexture.copy(QRect(-selected.value("offsetX").toInt(), -selected.value("offsetY").toInt(),
                  selected.value("width").toInt(), selected.value("height").toInt())),
            tile % 2 ? Qt::blue : Qt::green);
    }
    const double warmMs = warm.nsecsElapsed() / 1000000.0;
    QObject::disconnect(warmConnection);
    require(warmChanges == 0, "warm offset-only selections do not invalidate the preview descriptor");
    const QByteArray output = qgetenv("SPOOL_TRICKPLAY_SMOKE_OUTPUT");
    if (!output.isEmpty())
        require(first.save(QString::fromUtf8(output), "PNG"), "smoke output frame writes");
    std::cout << "trickplay fixture: old_full_sheet_decode_ms=" << baselineMs << " native_cold_response_ms=" << coldMs
              << " native_24_distinct_warm_frames_ms=" << warmMs << " old_texture_bytes=" << oldSheet.sizeInBytes()
              << " native_texture_bytes=" << firstTexture.sizeInBytes() << " output=" << output.constData() << '\n';
    // These are measured CPU response/decode times and texture byte counts,
    // not a claim that textureFactory creation measures actual GPU upload.
    const auto fastUrl = service.frame(0).value("url");
    service.setAccurateDecoding(true);
    require(service.frame(0).value("url") != fastUrl, "accuracy changes retire old decoded generations");
    auto accurateFactory = textureForSeconds(service, 0);
    auto *accurateBacking = dynamic_cast<TrickplayTextureFactory *>(accurateFactory.get());
    require(accurateBacking && accurateBacking->texture != firstBacking->texture,
        "accuracy changes do not reuse a cached fast transform");
    color(accurateFactory->image().copy(QRect(0, 0, 320, 180)), Qt::green);
    service.setAccurateDecoding(false);

    QImage redSheet(sheet.size(), QImage::Format_RGB32);
    redSheet.fill(Qt::red);
    const QByteArray secondSheet = jpeg(redSheet);
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost), "rapid-switch fixture server listens");
    QPointer<QTcpSocket> delayed;
    int secondSheetRequests = 0;
    const auto serve = [](QTcpSocket *socket, const QByteArray& bytes) {
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: "
            + QByteArray::number(bytes.size()) + "\r\nConnection: close\r\n\r\n" + bytes);
        socket->disconnectFromHost();
    };
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
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
                if (request.startsWith("GET /sheet/1.jpg ")) {
                    if (++secondSheetRequests == 1)
                        delayed = socket;
                    else
                        serve(socket, secondSheet);
                } else
                    serve(socket, encoded);
            });
        }
    });
    info.thumbnailCount = 200;
    info.urlTemplate = QStringLiteral("http://127.0.0.1:%1/sheet/{index}.jpg").arg(server.serverPort());
    service.setSession(info, 0);
    color(frame(service, 0), Qt::green);
    const QString delayedId = QUrl(service.frame(100).value("url").toString()).path().mid(1);
    std::unique_ptr<QQuickImageResponse> interrupted(service.requestImageResponse(delayedId));
    bool interruptedFinished = false;
    QObject::connect(interrupted.get(), &QQuickImageResponse::finished, [&] { interruptedFinished = true; });
    waitUntil([&] { return secondSheetRequests == 1; });
    color(frame(service, 25), Qt::blue); // CPU-cached sheet, not a cached frame.
    waitUntil([&] { return interruptedFinished; });
    require(!interrupted->errorString().isEmpty(), "returning to a cached sheet supersedes its pending fetch");
    waitUntil([&] { return !delayed || delayed->state() == QAbstractSocket::UnconnectedState; },
        "cached-sheet return must abort the stale HTTP transfer");
    color(frame(service, 100), Qt::red);
    color(frame(service, 26), Qt::green); // The encoded cache must not be poisoned.
    require(secondSheetRequests == 2, "cancelled sheet is fetched anew, never misfiled as another sheet");

    // Image-provider jobs may reach the service after a newer selection.
    secondSheetRequests = 0;
    service.setSession(info, 0);
    color(frame(service, 0), Qt::green);
    const QString oldId = QUrl(service.frame(0).value("url").toString()).path().mid(1);
    waitUntil([&] { return secondSheetRequests == 1; });
    const QString currentId = QUrl(service.frame(100).value("url").toString()).path().mid(1);
    std::unique_ptr<QQuickImageResponse> currentResponse(service.requestImageResponse(currentId));
    std::unique_ptr<QQuickImageResponse> lateResponse(service.requestImageResponse(oldId));
    bool currentFinished = false, lateFinished = false;
    QObject::connect(currentResponse.get(), &QQuickImageResponse::finished, [&] { currentFinished = true; });
    QObject::connect(lateResponse.get(), &QQuickImageResponse::finished, [&] { lateFinished = true; });
    waitUntil([&] { return lateFinished; });
    require(!lateResponse->errorString().isEmpty(), "late provider request cannot revive an old selection");
    require(!currentFinished && delayed && delayed->state() == QAbstractSocket::ConnectedState,
        "late provider request must not cancel the current foreground transfer");
    serve(delayed, secondSheet);
    waitUntil([&] { return currentFinished; });
    require(currentResponse->errorString().isEmpty(), "latest selection completes despite late provider delivery");
    std::unique_ptr<QQuickTextureFactory> currentTexture(currentResponse->textureFactory());
    require(bool(currentTexture), "latest selection delivers its texture");
    color(currentTexture->image(), Qt::red);

    info.urlTemplate = fixture(directory, "oversized.jpg", QByteArray(32 * 1024 * 1024 + 1, '\0'));
    service.setSession(info, 0);
    frame(service, 0, false);

    // Boundary cases distinguish resident delivery from clipped JPEG fallback,
    // including sheets above the former eight-megapixel cutoff.
    for (int height : { 250, 251 }) {
        TrickplayInfo boundary;
        boundary.width = 500;
        boundary.height = height;
        boundary.tileWidth = boundary.tileHeight = 10;
        boundary.thumbnailCount = 100;
        boundary.intervalMs = 1000;
        QImage large(5000, height * 10, QImage::Format_RGB32);
        large.fill(Qt::green);
        QPainter tiles(&large);
        tiles.fillRect(4500, height * 9, 500, height, Qt::blue);
        tiles.end();
        boundary.urlTemplate = fixture(directory, height == 250 ? "boundary.jpg" : "clipped.jpg", jpeg(large));
        service.setSession(boundary, 99);
        const auto last = service.frame(99);
        const auto first = service.frame(0);
        const bool resident = height == 250;
        require((last.value("url") == first.value("url")) == resident,
            "50 decimal MB boundary chooses stable sheet URLs or clipped frame URLs");
        require(last.value("sheetWidth").toInt() == (resident ? 5000 : 500)
                && last.value("sheetHeight").toInt() == (resident ? 2500 : 251),
            "50 decimal MB boundary chooses correct texture dimensions");
        color(frame(service, 99), Qt::blue);
        color(frame(service, 0), Qt::green);
    }

    info = {};
    info.width = 32;
    info.height = 18;
    info.tileWidth = info.tileHeight = info.thumbnailCount = 1;
    info.intervalMs = 1000;
    info.urlTemplate = fixture(directory, "wrong-geometry.jpg", jpeg(blue));
    service.setSession(info, 0);
    frame(service, 0, false);

    QTcpServer windowServer;
    require(windowServer.listen(QHostAddress::LocalHost), "directional prefetch fixture listens");
    QImage smallSheet(64, 18, QImage::Format_RGB32);
    smallSheet.fill(Qt::red);
    const QByteArray smallBytes = jpeg(smallSheet);
    int sheetRequests[4] = {};
    QPointer<QTcpSocket> speculativeSocket;
    QObject::connect(&windowServer, &QTcpServer::newConnection, &windowServer, [&] {
        while (auto *socket = windowServer.nextPendingConnection()) {
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
                bool ok = false;
                const int sheetIndex = request.split(' ').value(1).split('/').last().split('.').first().toInt(&ok);
                require(ok && sheetIndex >= 0 && sheetIndex < 4, "prefetch never requests beyond descriptor bounds");
                ++sheetRequests[sheetIndex];
                if (sheetIndex == 1)
                    speculativeSocket = socket;
                else
                    serve(socket, smallBytes);
            });
        }
    });
    info.width = 32;
    info.height = 18;
    info.tileWidth = 2;
    info.tileHeight = 1;
    info.thumbnailCount = 8;
    info.urlTemplate = QStringLiteral("http://127.0.0.1:%1/window/{index}.jpg").arg(windowServer.serverPort());
    service.setSession(info, 0);
    color(frame(service, 0), Qt::red);
    waitUntil([&] { return sheetRequests[1] == 1; }, "hover starts one forward speculative sheet");
    color(frame(service, 6), Qt::red);
    waitUntil([&] { return !speculativeSocket || speculativeSocket->state() == QAbstractSocket::UnconnectedState; },
        "latest foreground aborts a stalled speculative transfer");
    require(sheetRequests[3] == 1, "foreground reaches latest sheet while speculation is stalled");
    const auto forward = service.frame(7);
    const auto backward = service.frame(6);
    require(forward.value("url") == backward.value("url"), "direction reversal can be offset-only");
    waitUntil([&] { return sheetRequests[2] == 1; }, "offset-only reversal prefetches backward neighbour");
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 100) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(sheetRequests[0] == 1 && sheetRequests[1] == 1 && sheetRequests[2] == 1 && sheetRequests[3] == 1,
        "speculative completion does not walk the timeline or retry abandoned neighbours");
    service.setSession(info, 0);
    color(frame(service, 0), Qt::red);
    waitUntil([&] { return sheetRequests[1] == 2; });
    service.clear();
    waitUntil([&] { return !speculativeSocket || speculativeSocket->state() == QAbstractSocket::UnconnectedState; },
        "session clear aborts speculative transfers");
    require(!service.frame(0).value("available").toBool(), "cleared prefetch cannot revive the session");
    return 0;
}
