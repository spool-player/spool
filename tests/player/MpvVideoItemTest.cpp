#include "player/MpvVideoItem.h"

#include "platform/PlatformDisplayOutput.h"
#include "player/RenderTargetProfile.h"

#include "TestMain.h"

#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QTemporaryFile>
#include <QThread>

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

bool containsVideoPixel(const QImage& image)
{
    if (image.isNull())
        return false;
    for (int y = 0; y < image.height(); y += 8) {
        for (int x = 0; x < image.width(); x += 8) {
            if (isRed(image.pixelColor(x, y)) || isBlue(image.pixelColor(x, y)))
                return true;
        }
    }
    return false;
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
    return redAbove > blueAbove && blueBelow > redBelow;
}

} // namespace

JELLYFIN_TEST_MAIN("mpv-video-item")
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
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication app(argc, argv);

    QTemporaryFile video(QDir::tempPath() + QStringLiteral("/mpv-video-item-XXXXXX.mkv"));
    if (!writeVideo(video)) {
        std::fprintf(stderr, "failed to create test video\n");
        return 1;
    }

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(320, 180);
    JellyfinNative::MpvVideoItem videoItem(window.contentItem());
    videoItem.setSize(QSizeF(window.size()));
    window.show();
    app.processEvents();

    // Reusing an item after detach must reset first-frame state and publish
    // the new context, including when Qt replaces the render target on resize.
    for (const QSize size : { QSize(320, 180), QSize(480, 270) }) {
        window.resize(size);
        videoItem.setSize(QSizeF(size));
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
            || mpv_initialize(handle) < 0) {
            std::fprintf(stderr, "failed to initialize mpv\n");
            if (handle)
                mpv_terminate_destroy(handle);
            return 1;
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

        bool rendered = false;
        QElapsedTimer timer;
        timer.start();
        while (!rendered && timer.elapsed() < 5000) {
            app.processEvents(QEventLoop::AllEvents, 20);
            rendered = containsVideoPixel(window.grabWindow());
            QThread::msleep(10);
        }

        // Diagnostic, not an assertion: what the swapchain can present depends on
        // the driver, the compositor and whether the display is in HDR mode, none
        // of which a test can require.
        const JellyfinNative::DisplayOutputCapabilities display = JellyfinNative::PlatformDisplayOutput::probe(&window);
        std::fprintf(stderr, "display: hdrAvailable=%d format=%d sdrWhite=%.0f min=%.4f max=%.0f\n",
            int(display.hdrAvailable), int(display.preferredFormat), double(display.sdrWhiteNits),
            double(display.minLuminanceNits), double(display.maxLuminanceNits));

        const bool upright = rendered && isRightWayUp(window.grabWindow());
        const bool released = videoItem.releaseMpvHandle();
        mpv_terminate_destroy(handle);
        if (!rendered || !upright || !released) {
            std::fprintf(stderr, "video result: rendered=%d upright=%d released=%d\n", rendered, upright, released);
            return 1;
        }
    }
    return 0;
}

namespace {

int vulkanEntry(int argc, char **argv)
{
    qputenv("SPOOL_TEST_RENDER_API", "vulkan");
    return jellyfinTestBody(argc, argv);
}

// Registered by hand rather than with a second JELLYFIN_TEST_MAIN, which names
// its body the same thing every time and so can only appear once per file.
[[maybe_unused]] const bool vulkanRegistered = ::JellyfinTests::registerTest("mpv-video-item-vulkan", &vulkanEntry);

} // namespace
