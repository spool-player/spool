#include "LocalThumbnail.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSaveFile>
#include <mpv/client.h>
#include <mpv/render.h>

#include <memory>

namespace Spool {
QByteArray localThumbnail(const QUrl& url, const QString& cacheDirectory, QString& error)
{
    QUrl fileUrl(url);
    fileUrl.setScheme(QStringLiteral("file"));
    fileUrl.setQuery({});
    const QFileInfo source(fileUrl.toLocalFile());
    if (!source.isFile() || !source.isReadable()) {
        error = QStringLiteral("The local media file is unavailable");
        return {};
    }
    const QByteArray identity = source.canonicalFilePath().toUtf8() + '\n' + QByteArray::number(source.size()) + '\n'
        + QByteArray::number(source.lastModified().toMSecsSinceEpoch());
    const QString directory = QDir(cacheDirectory).filePath(QStringLiteral("local-thumbnails-v1"));
    QDir().mkpath(directory);
    const QString destination = QDir(directory).filePath(
        QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex())
        + QStringLiteral(".jpg"));
    QFile cached(destination);
    if (cached.open(QIODevice::ReadOnly))
        return cached.readAll();

    std::unique_ptr<mpv_handle, decltype(&mpv_terminate_destroy)> decoder(mpv_create(), &mpv_terminate_destroy);
    if (!decoder) {
        error = QStringLiteral("Could not create the thumbnail decoder");
        return {};
    }
    const auto option
        = [&](const char *name, const char *value) { return mpv_set_option_string(decoder.get(), name, value) >= 0; };
    if (!option("config", "no") || !option("terminal", "no") || !option("load-scripts", "no") || !option("audio", "no")
        || !option("sub", "no") || !option("hwdec", "no") || !option("vd-lavc-threads", "1") || !option("vo", "libmpv")
        || !option("pause", "yes") || !option("start", "10%") || mpv_initialize(decoder.get()) < 0) {
        error = QStringLiteral("Could not initialize the thumbnail decoder");
        return {};
    }
    // The shipped FFmpeg deliberately contains no image encoders or scale
    // filter. libmpv's software renderer uses swscale directly; Qt encodes the
    // bounded result. No GPU, external ffmpeg executable or visible window.
    mpv_render_param creation[] = { { MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW) },
        { MPV_RENDER_PARAM_INVALID, nullptr } };
    mpv_render_context *rawContext = nullptr;
    if (mpv_render_context_create(&rawContext, decoder.get(), creation) < 0) {
        error = QStringLiteral("Could not create the software thumbnail renderer");
        return {};
    }
    std::unique_ptr<mpv_render_context, decltype(&mpv_render_context_free)> renderer(
        rawContext, &mpv_render_context_free);
    const QByteArray path = source.absoluteFilePath().toUtf8();
    const char *command[] = { "loadfile", path.constData(), nullptr };
    if (mpv_command(decoder.get(), command) < 0) {
        error = QStringLiteral("Could not open media for a thumbnail");
        return {};
    }
    QImage image;
    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < 10000) {
        const mpv_event *event = mpv_wait_event(decoder.get(), 0.01);
        if (event->event_id == MPV_EVENT_END_FILE || event->event_id == MPV_EVENT_SHUTDOWN)
            break;
        if (!(mpv_render_context_update(renderer.get()) & MPV_RENDER_UPDATE_FRAME))
            continue;
        mpv_render_frame_info frame {};
        mpv_render_context_get_info(renderer.get(), { MPV_RENDER_PARAM_NEXT_FRAME_INFO, &frame });
        int64_t width = 0, height = 0;
        if (mpv_get_property(decoder.get(), "dwidth", MPV_FORMAT_INT64, &width) < 0
            || mpv_get_property(decoder.get(), "dheight", MPV_FORMAT_INT64, &height) < 0 || width <= 0 || height <= 0
            || width > 32768 || height > 32768)
            continue;
        const QSize size
            = QSize(static_cast<int>(width), static_cast<int>(height)).scaled(640, 360, Qt::KeepAspectRatio);
        image = QImage(size, QImage::Format_RGBX8888);
        int dimensions[] = { image.width(), image.height() };
        size_t stride = image.bytesPerLine();
        mpv_render_param parameters[] = { { MPV_RENDER_PARAM_SW_SIZE, dimensions },
            { MPV_RENDER_PARAM_SW_FORMAT, const_cast<char *>("rgb0") }, { MPV_RENDER_PARAM_SW_STRIDE, &stride },
            { MPV_RENDER_PARAM_SW_POINTER, image.bits() }, { MPV_RENDER_PARAM_INVALID, nullptr } };
        if (mpv_render_context_render(renderer.get(), parameters) < 0) {
            image = {};
            break;
        }
        if ((frame.flags & MPV_RENDER_FRAME_INFO_PRESENT) && !(frame.flags & MPV_RENDER_FRAME_INFO_REDRAW))
            break;
        image = {};
    }
    if (image.isNull()) {
        error = QStringLiteral("No video frame could be decoded for this file");
        return {};
    }
    QByteArray bytes;
    QBuffer encoded(&bytes);
    if (!encoded.open(QIODevice::WriteOnly) || !image.save(&encoded, "JPEG", 82)) {
        error = QStringLiteral("Could not encode the local thumbnail");
        return {};
    }
    QSaveFile saved(destination);
    if (saved.open(QIODevice::WriteOnly) && saved.write(bytes) == bytes.size())
        saved.commit();
    return bytes;
}
}
