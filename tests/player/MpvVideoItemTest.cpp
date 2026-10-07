#include "player/MpvVideoItem.h"

#include "platform/PlatformDisplayOutput.h"
#include "player/MpvOptionProfile.h"
#include "player/RenderTargetProfile.h"

#include "TestMain.h"

#include <QDir>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QQuickWindow>
#include <QSGTexture>
#include <QSGTextureProvider>
#include <QSurfaceFormat>
#include <QTemporaryFile>
#include <QTimer>
#include <algorithm>
#include <atomic>
#if SPOOL_MPV_ITEM_RHI
#include <rhi/qrhi.h>
#endif

#include <clocale>
#include <cstdio>

extern "C" {
#include <mpv/client.h>
}

namespace {

bool writeVideo(QTemporaryFile& file)
{
    // A red top half over a blue bottom half, so the grab below can tell which
    // way up the frame arrived. A uniform frame cannot: the scene graph samples
    // the item's texture with a different origin convention than an OpenGL
    // framebuffer writes it, and getting that wrong is invisible until the
    // picture is upside down.
    static const QByteArray encoded(
        "GkXfo6NChoEBQveBAULygQRC84EIQoKIbWF0cm9za2FCh4EEQoWBAhhTgGcBAAAAAAADcBFNm3TAv4TOvihrTbuLU6uEFUmpZlOsgaFNu4tTq4"
        "QWVK5rU6yB7027jFOrhBJUw2dTrIIBgE27jFOrhBxTu2tTrIIDVOwBAAAAAAAAUwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAFUmpZsm/hGlzfewq17GDD0JATYCMTGF2ZjYzLjEuMTAxV0"
        "GMTGF2ZjYzLjEuMTAxc6SQreZwOuxO0yMxY2vMVQNY0USJiEBpAAAAAAAAFlSua0CLv4RetviWrgEAAAAAAAB814EBc8WIVxEvauyQpzCcgQAi"
        "tZyDdW5kiIEAhoZWX0ZGVjGDgQEj44OEBfXhAOCQsIFAuoFAmoECVbCEVbmBAVXugQDsAQAAAAAAAAIAAGOiqlYrhNGcBS9BPGAm6Vw3b10bdp"
        "edOsnEIEMei59VIFEvTvihaDubFxN8AxJUw2f+v4SfHzw3c3OfY8CAZ8iZRaOHRU5DT0RFUkSHjExhdmY2My4xLjEwMXNz02PAi2PFiFcRL2rs"
        "kKcwZ8ieRaOHRU5DT0RFUkSHkUxhdmM2My4xLjEwMSBmZnYxZ8ihRaOIRFVSQVRJT05Eh5MwMDowMDowMC4yMDAwMDAwMDAAH0O2dUFLv4R0cs"
        "8T54EAo0CogQAAgPwVgAAErK////+f///////AAU8r/+f///+wyv/5////4AAAIQAzUIGfPd6AAASsr////5///////8ABTyv/5////7DK//n/"
        "///gAAAhAE7SlqucHJMAAiyv////n///////wDyv/5////4ADRlf/z////wAACEAmd4MahC/PQACLK////+f///////APK//n////gANGV//P/"
        "///AAAIQDxKHhfo0CUgQBkAHyVgBe/////3///////xz//9////7D//7////wAABwA9Go9cz3egBe/////3///////xz//9////7D//7////wA"
        "ABwAYZzUf5wckxd/////v///////r///v////C7//7////wAABwAzwS8hhC/PRd/////v///////r///v////C7//7////wAABwAa5JS6xxTu2"
        "uXv4TNfoVtu4+zgQC3iveBAfGCAgPwgQk=");
    const QByteArray video = QByteArray::fromBase64(encoded);
    return file.open() && file.write(video) == video.size() && file.flush();
}

bool isRed(const QColor& color)
{
    return color.red() > 80 && color.red() > color.green() * 2 && color.red() > color.blue() * 2;
}

bool isBlue(const QColor& color)
{
    return color.blue() > 80 && color.blue() > color.green() * 2 && color.blue() > color.red() * 2;
}

bool containsNeutralOsd(const QImage& image, const QImage& baseline)
{
    if (image.size() != baseline.size())
        return false;
    int neutral = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            // Check text over the fixture's black letterbox, not antialiased
            // text blended into the red/blue video or a particular font weight.
            if (baseline.pixelColor(x, y) != QColor(Qt::black))
                continue;
            const QColor color = image.pixelColor(x, y);
            const int high = std::max({ color.red(), color.green(), color.blue() });
            const int low = std::min({ color.red(), color.green(), color.blue() });
            if (high <= 16)
                continue;
            if (high - low > 8)
                return false;
            ++neutral;
        }
    }
    return neutral >= 8;
}

// The video is red over blue, so the frame is the right way up when the top of
// the window is red and the bottom is blue. Counting rather than sampling one
// pixel keeps letterboxing and the window's own background out of the answer.
bool isRightWayUp(const QImage& image)
{
    if (image.isNull())
        return false;
    int redAbove = 0;
    int blueAbove = 0;
    int redBelow = 0;
    int blueBelow = 0;
    const int middle = image.height() / 2;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); x += 4) {
            const QColor color = image.pixelColor(x, y);
            if (isRed(color))
                (y < middle ? redAbove : redBelow)++;
            else if (isBlue(color))
                (y < middle ? blueAbove : blueBelow)++;
        }
    }
    std::fprintf(stderr, "orientation: redAbove=%d blueAbove=%d redBelow=%d blueBelow=%d\n", redAbove, blueAbove,
        redBelow, blueBelow);
    return redAbove > blueAbove && blueBelow > redBelow
        && isRed(image.pixelColor(image.width() / 2, image.height() / 4))
        && isBlue(image.pixelColor(image.width() / 2, 3 * image.height() / 4));
}

} // namespace

SPOOL_TEST_MAIN("mpv-video-item")
{
    // The same end-to-end check is worth running against either backend, and
    // the Vulkan one is the whole reason the item moved to the RHI. OpenGL
    // stays the default so CI and a plain local run test what ships.
    const QByteArray api = qgetenv("SPOOL_TEST_RENDER_API").toLower();
#if QT_CONFIG(vulkan)
    if (api == "vulkan")
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    else
#endif
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    if (api == "vulkan")
        std::fprintf(stderr, "requested Vulkan scene graph\n");
    QSurfaceFormat format;
#ifdef Q_OS_TVOS
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(3, 0);
#else
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
#endif
    format.setAlphaBufferSize(0);
    QGuiApplication app(argc, argv);

    QTemporaryFile video(QDir::tempPath() + QStringLiteral("/mpv-video-item-XXXXXX.mkv"));
    if (!writeVideo(video)) {
        std::fprintf(stderr, "failed to create test video\n");
        return 1;
    }

#if SPOOL_MPV_ITEM_RHI
    std::atomic_int textureFormat { -1 };
#endif
    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(320, 180);
    Spool::MpvVideoItem videoItem(window.contentItem());
    // Match production's anchors.fill: parent. UIKit can replace requested
    // window geometry asynchronously with the fullscreen television surface.
    qreal viewportFraction = 1.0;
    const auto fitSurface = [&] { videoItem.setSize(window.contentItem()->size() * viewportFraction); };
    QObject::connect(window.contentItem(), &QQuickItem::widthChanged, &videoItem, fitSurface);
    QObject::connect(window.contentItem(), &QQuickItem::heightChanged, &videoItem, fitSurface);
    fitSurface();
#if SPOOL_MPV_ITEM_RHI
    QObject::connect(
        &window, &QQuickWindow::afterRendering, &videoItem,
        [&] {
            const auto *provider = videoItem.textureProvider();
            const auto *texture = provider ? provider->texture() : nullptr;
            if (const auto *target = texture ? texture->rhiTexture() : nullptr)
                textureFormat.store(int(target->format()));
        },
        Qt::DirectConnection);
#endif
#ifdef Q_OS_TVOS
    window.showFullScreen();
#else
    window.show();
#endif
    app.processEvents();
    const auto captureItem = [&] {
        const QImage image = window.grabWindow();
#ifdef Q_OS_TVOS
        if (image.isNull())
            return image;
        const qreal xScale = qreal(image.width()) / window.contentItem()->width();
        const qreal yScale = qreal(image.height()) / window.contentItem()->height();
        return image.copy(0, 0, qRound(videoItem.width() * xScale), qRound(videoItem.height() * yScale));
#else
        return image;
#endif
    };
    const auto waitForPresentedFrame = [&](const auto& matches) {
        bool matched = false;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(
            &window, &QQuickWindow::frameSwapped, &loop,
            [&] {
                if (!matched && matches(captureItem())) {
                    matched = true;
                    loop.quit();
                }
            },
            Qt::QueuedConnection);
        timeout.start(5000);
        // UIKit returns to UIApplicationMain only for EventLoopExec, not a
        // manual processEvents pump. Read back after its real presentation.
        loop.exec();
        return matched;
    };

    // Reusing an item after detach must reset first-frame state and publish
    // the new context, including when Qt replaces the render target on resize.
    for (const QSize size : { QSize(320, 180), QSize(480, 270) }) {
#ifdef Q_OS_TVOS
        // UIKit owns the fullscreen native window; resize the real video
        // viewport instead of requesting an unsupported television window size.
        viewportFraction = size.width() == 320 ? 1.0 : 2.0 / 3.0;
#else
        window.resize(size);
#endif
        fitSurface();
        std::setlocale(LC_NUMERIC, "C");
        mpv_handle *handle = mpv_create();
        const bool verbose = !qgetenv("SPOOL_TEST_MPV_LOG").isEmpty();
        // mpv_create can fail, and the check for that is below: setting options on
        // its result first would crash instead of reporting it, but only for a run
        // that asked for logging.
        if (handle && verbose
            && (mpv_set_option_string(handle, "terminal", "yes") < 0
                || mpv_set_option_string(handle, "msg-level", "all=debug") < 0)) {
            std::fprintf(stderr, "failed to enable mpv logging\n");
            return 1;
        }
        if (!handle || mpv_set_option_string(handle, "terminal", verbose ? "yes" : "no") < 0
            || mpv_set_option_string(handle, "vo", "libmpv") < 0 || mpv_set_option_string(handle, "hwdec", "no") < 0
            || mpv_set_option_string(handle, "loop-file", "inf") < 0
            || mpv_set_option_string(handle, "osd-color", "#FFFFFFFF") < 0
            || mpv_set_option_string(handle, "osd-font-size", "48") < 0 || mpv_initialize(handle) < 0) {
            std::fprintf(stderr, "failed to initialize mpv\n");
            if (handle)
                mpv_terminate_destroy(handle);
            return 1;
        }
        for (const auto& option : Spool::RenderTargetPolicy::targetOptions({})) {
            if (mpv_set_option_string(handle, option.name.constData(), option.value.constData()) < 0) {
                std::fprintf(stderr, "failed to set the managed SDR target\n");
                mpv_terminate_destroy(handle);
                return 1;
            }
        }

        videoItem.setRenderBackend(qgetenv("SPOOL_TEST_RENDER_BACKEND"));
        videoItem.setMpvHandle(handle);
        if (!videoItem.waitForRenderContext()) {
            std::fprintf(stderr, "render context was not ready before media load\n");
            mpv_terminate_destroy(handle);
            return 1;
        }

        const QByteArray path = QFile::encodeName(video.fileName());
        const char *command[] = { "loadfile", path.constData(), nullptr };
        if (mpv_command(handle, command) < 0) {
            std::fprintf(stderr, "failed to load test video\n");
            videoItem.releaseMpvHandle();
            mpv_terminate_destroy(handle);
            return 1;
        }

        const bool rendered = waitForPresentedFrame(isRightWayUp);
        if (!rendered) {
            int64_t decodedWidth = 0;
            int64_t decodedHeight = 0;
            double position = -1;
            int eof = -1;
            const int widthStatus = mpv_get_property(handle, "video-params/w", MPV_FORMAT_INT64, &decodedWidth);
            const int heightStatus = mpv_get_property(handle, "video-params/h", MPV_FORMAT_INT64, &decodedHeight);
            const int positionStatus = mpv_get_property(handle, "time-pos", MPV_FORMAT_DOUBLE, &position);
            const int eofStatus = mpv_get_property(handle, "eof-reached", MPV_FORMAT_FLAG, &eof);
            std::fprintf(stderr, "decoder: width=%lld(%d) height=%lld(%d) position=%.3f(%d) eof=%d(%d)\n",
                static_cast<long long>(decodedWidth), widthStatus, static_cast<long long>(decodedHeight), heightStatus,
                position, positionStatus, eof, eofStatus);
            captureItem().save(QDir::tempPath() + QStringLiteral("/mpv-video-item-failure.png"));
        }

        // Diagnostic, not an assertion: what the swapchain can present depends on
        // the driver, the compositor and whether the display is in HDR mode, none
        // of which a test can require.
        const Spool::DisplayOutputCapabilities display = Spool::PlatformDisplayOutput::probe(&window);
        std::fprintf(stderr, "display: hdrAvailable=%d format=%d sdrWhite=%.0f min=%.4f max=%.0f\n",
            int(display.hdrAvailable), int(display.preferredFormat), double(display.sdrWhiteNits),
            double(display.minLuminanceNits), double(display.maxLuminanceNits));

        const bool upright = rendered && isRightWayUp(captureItem());
        char *pixelFormat = mpv_get_property_string(handle, "video-target-params/pixelformat");
        const QByteArray actualFormat = pixelFormat ? QByteArray(pixelFormat) : QByteArray();
        mpv_free(pixelFormat);
        // The legacy OpenGL renderer does not expose video-target-params.
        // Inspect the actual RHI texture on both APIs, and additionally the
        // mpv handover descriptor on Vulkan.
#if SPOOL_MPV_ITEM_RHI
        const bool sdrTarget
            = textureFormat.load() == int(QRhiTexture::RGBA8) && (api != "vulkan" || actualFormat == "rgba8");
        std::fprintf(stderr, "rendered SDR target: RHI=%d mpv=%s\n", textureFormat.load(),
            actualFormat.isEmpty() ? "(legacy renderer)" : actualFormat.constData());
#endif
        const QImage beforeOsd = captureItem();
        const char *osdCommand[] = { "show-text", "SDR white", "10000", nullptr };
        bool neutralOsd = false;
        if (mpv_command(handle, osdCommand) >= 0) {
            neutralOsd = waitForPresentedFrame([&](const QImage& image) { return containsNeutralOsd(image, beforeOsd); });
        }
        const bool released = videoItem.releaseMpvHandle();
        mpv_terminate_destroy(handle);
        if (!rendered || !upright || !released || !neutralOsd
#if SPOOL_MPV_ITEM_RHI
            || !sdrTarget
#endif
        ) {
            std::fprintf(stderr, "video result: rendered=%d upright=%d released=%d neutralOSD=%d\n",
                rendered, upright, released, neutralOsd);
            const QImage failedFrame = captureItem();
            const QColor upper = failedFrame.pixelColor(failedFrame.width() / 2, failedFrame.height() / 4);
            const QColor lower = failedFrame.pixelColor(failedFrame.width() / 2, 3 * failedFrame.height() / 4);
            std::fprintf(stderr, "viewport: window=%dx%d content=%.0fx%.0f item=%.0fx%.0f capture=%dx%d upper=%s lower=%s\n",
                window.width(), window.height(), window.contentItem()->width(), window.contentItem()->height(),
                videoItem.width(), videoItem.height(), failedFrame.width(), failedFrame.height(),
                qPrintable(upper.name()), qPrintable(lower.name()));
            return 1;
        }
    }
    std::fprintf(stderr, "mpv video smoke: upright frames and OSD rendered across detach and resize\n");
    return 0;
}

namespace {

int vulkanEntry(int argc, char **argv)
{
    qputenv("SPOOL_TEST_RENDER_API", "vulkan");
    return spoolTestBody(argc, argv);
}

// Registered by hand rather than with a second SPOOL_TEST_MAIN, which names
// its body the same thing every time and so can only appear once per file.
[[maybe_unused]] const bool vulkanRegistered = ::SpoolTests::registerTest("mpv-video-item-vulkan", &vulkanEntry);

} // namespace
