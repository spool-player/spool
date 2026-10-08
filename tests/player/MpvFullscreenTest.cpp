#include "TestMain.h"
#include "platform/NativeAppWindow.h"
#include "player/MpvVideoItem.h"
#include "player/PlayerController.h"

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QScreen>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <clocale>
#include <cstdio>
#include <cstdlib>

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(EXIT_FAILURE);
    }
}

template <typename Predicate> bool waitUntil(Predicate ready, int timeout = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return ready();
}

void processFor(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    waitUntil([&] { return timer.elapsed() >= milliseconds; }, milliseconds + 1000);
}

} // namespace

SPOOL_TEST_MAIN("mpv-video-item-fullscreen")
{
    const bool vulkan = qgetenv("SPOOL_TEST_RENDER_API") == "vulkan";
    QQuickWindow::setGraphicsApi(vulkan ? QSGRendererInterface::Vulkan : QSGRendererInterface::OpenGL);
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setAlphaBufferSize(0);
    QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication app(argc, argv);
    std::setlocale(LC_NUMERIC, "C");

    QTemporaryDir configuration;
    require(configuration.isValid(), "fullscreen configuration directory opens");
    QFile config(configuration.filePath(QStringLiteral("mpv.conf")));
    require(config.open(QIODevice::WriteOnly) && config.write("ao=null\nhwdec=no\nloop-file=inf\n") > 0,
        "fullscreen playback configuration writes");
    config.close();
    QFile bindings(configuration.filePath(QStringLiteral("input.conf")));
    require(bindings.open(QIODevice::WriteOnly) && bindings.write("F9 set fullscreen yes\nF10 set fullscreen no\n") > 0,
        "custom fullscreen bindings write");
    bindings.close();

    Spool::NativeAppWindow window(QStringLiteral("fullscreen-test"));
    const QSize normalSize = window.screen()->availableSize() / 2;
    window.resize(normalSize);
    Spool::MpvVideoItem video(window.contentItem());
    video.setSize(QSizeF(window.size()));
    QObject::connect(&window, &QWindow::widthChanged, &video, [&] { video.setWidth(window.width()); });
    QObject::connect(&window, &QWindow::heightChanged, &video, [&] { video.setHeight(window.height()); });
    Spool::PlayerController player(&window, nullptr, nullptr, {});
    player.setMpvConfigPolicy({ Spool::MpvConfigPolicy::Mode::Custom, configuration.path() });

    std::atomic<int> renderedWidth { 0 };
    std::atomic<int> swappedWidth { 0 };
    std::atomic<int> swaps { 0 };
    QObject::connect(
        &window, &QQuickWindow::afterRendering, &window, [&] { renderedWidth.store(window.width()); },
        Qt::DirectConnection);
    QObject::connect(
        &window, &QQuickWindow::frameSwapped, &window,
        [&] {
            swappedWidth.store(renderedWidth.load());
            ++swaps;
        },
        Qt::DirectConnection);
    window.show();
    require(waitUntil([&] { return swaps.load() > 0; }), "native window presents its first frame");

    bool responsive = true;
    const auto transitions = [&](const char *state) {
        for (int index = 0; index < 6; ++index) {
            const bool entering = !window.fullScreen();
            const int fullscreenWidth = window.screen()->size().width();
            const int previousSwaps = swaps.load();
            QElapsedTimer timer;
            timer.start();
            window.toggleFullScreen();
            const double callMs = timer.nsecsElapsed() / 1e6;
            // The VO's 200 ms timeout must never become the GUI's fullscreen
            // latency. Swap timing is evidence, not a compositor speed promise.
            responsive &= callMs < 150.0;
            const bool presented = waitUntil([&] {
                // Wayland client decorations can alter the restored content
                // size. Require its configured frame, not a guessed border size.
                const bool configured = entering ? window.width() == fullscreenWidth
                                                 : window.width() > 0 && window.width() < fullscreenWidth;
                return configured && window.fullScreen() == entering && swaps.load() > previousSwaps
                    && swappedWidth.load() == window.width();
            });
            if (!presented)
                std::fprintf(stderr,
                    "fullscreen predicate: entering=%d actual_full=%d width=%d full_width=%d swaps=%d previous=%d "
                    "swapped_width=%d exposed=%d visibility=%d\n",
                    entering, window.fullScreen(), window.width(), fullscreenWidth, swaps.load(), previousSwaps,
                    swappedWidth.load(), window.isExposed(), int(window.visibility()));
            require(presented, "fullscreen transition presents a frame at the configured size");
            std::fprintf(stderr, "fullscreen: api=%s state=%s direction=%s call_ms=%.3f swap_ms=%.3f\n",
                vulkan ? "Vulkan" : "OpenGL", state, entering ? "enter" : "exit", callMs, timer.nsecsElapsed() / 1e6);
            processFor(100);
        }
    };
    transitions("idle");

    Spool::PlaybackSession session;
    session.itemId = QStringLiteral("fullscreen-fixture");
    session.title = session.itemId;
    session.itemType = QStringLiteral("Movie");
    session.url = QUrl::fromLocalFile(QStringLiteral(TEST_SOURCE_DIR "/tests/fixtures/local-thumbnail.mkv")).toString();
    session.playMethod = QStringLiteral("DirectPlay");
    player.play(session);
    require(waitUntil([&] { return player.fileLoaded(); }), "embedded video loads");
    processFor(500);
    transitions("playing");
    player.setPaused(true);
    require(waitUntil([&] { return player.paused(); }), "pause is acknowledged");
    transitions("paused");

    // Older asynchronous echoes must not reverse the latest native request.
    const bool original = window.fullScreen();
    for (int index = 0; index < 10; ++index) {
        window.toggleFullScreen();
        window.toggleFullScreen();
    }
    processFor(500);
    require(window.fullScreen() == original, "rapid native requests preserve their final fullscreen state");
    const auto press = [&](int key) {
        require(player.forwardMpvKey(key, Qt::NoModifier, {}, true, false), "custom mpv key press is dispatched");
        require(player.forwardMpvKey(key, Qt::NoModifier, {}, false, false), "custom mpv key release is dispatched");
    };
    press(Qt::Key_F9);
    require(waitUntil([&] { return window.fullScreen(); }), "custom mpv binding enters native fullscreen");
    processFor(100);
    require(window.fullScreen(), "fullscreen echo does not reverse a custom mpv binding");
    press(Qt::Key_F10);
    require(waitUntil([&] { return !window.fullScreen(); }), "custom mpv binding leaves native fullscreen");
    processFor(100);
    require(!window.fullScreen(), "windowed echo does not reverse a custom mpv binding");

    player.stop();
    transitions("stopped");
    player.teardownMpv();
    require(responsive, "fullscreen never blocks the GUI awaiting mpv's render timeout");
    return EXIT_SUCCESS;
}

#ifndef SPOOL_TEST_OPENGL_ONLY
namespace {

int vulkanEntry(int argc, char **argv)
{
    qputenv("SPOOL_TEST_RENDER_API", "vulkan");
    return spoolTestBody(argc, argv);
}

[[maybe_unused]] const bool vulkanRegistered
    = ::SpoolTests::registerTest("mpv-video-item-fullscreen-vulkan", &vulkanEntry);

} // namespace
#endif
