#include "TestMain.h"
#include "TestRequire.h"
#include "platform/NativeAppWindow.h"
#include "player/MpvVideoItem.h"
#include "player/PlayerController.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSurfaceFormat>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <clocale>
#include <cstdlib>
#include <functional>

using namespace Spool;
using SpoolTests::require;

namespace {
// Readiness/adoption diagnostics attribute the benchmark, not the playback
// assertion: every sample must still load and present real embedded video.
std::atomic<int> preparedCount { 0 };
std::atomic<int> adoptedCount { 0 };
std::atomic<qint64> preparationMs { -1 };
QtMessageHandler previousHandler = nullptr;

void messages(QtMsgType type, const QMessageLogContext& context, const QString& text)
{
    if (text.startsWith(QLatin1String("player: idle-prepared mpv initialized in"))) {
        const QStringList words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        bool valid = false;
        const qint64 elapsed = words.value(words.size() - 2).toLongLong(&valid);
        preparationMs.store(valid ? elapsed : -1);
        ++preparedCount;
    }
    if (text.startsWith(QLatin1String("player: adopting idle-prepared mpv handle")))
        ++adoptedCount;
    if (previousHandler)
        previousHandler(type, context, text);
}

bool waitUntil(const std::function<bool()>& ready, int timeout = 30000)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return ready();
}

class Heartbeat {
public:
    Heartbeat()
    {
        clock.start();
        timer.setTimerType(Qt::PreciseTimer);
        timer.setInterval(10);
        QObject::connect(&timer, &QTimer::timeout, &timer, [this] { tick(); });
        timer.start();
    }
    void tick()
    {
        const double now = clock.nsecsElapsed() / 1e6;
        maxGap = std::max(maxGap, now - last);
        last = now;
        ++ticks;
    }
    void reset()
    {
        maxGap = 0;
        ticks = 0;
        last = clock.nsecsElapsed() / 1e6;
    }
    QTimer timer;
    QElapsedTimer clock;
    double last = 0;
    double maxGap = 0;
    int ticks = 0;
};

void writeConfig(const QString& directory, int volume)
{
    QFile file(directory + QStringLiteral("/mpv.conf"));
    const QByteArray config = "ao=null\nhwdec=no\nloop-file=inf\nvolume=" + QByteArray::number(volume) + '\n';
    require(file.open(QIODevice::WriteOnly) && file.write(config) == config.size(),
        "benchmark custom mpv configuration writes");
}

PlaybackSession session()
{
    PlaybackSession result;
    result.itemId = QStringLiteral("setup-benchmark");
    result.title = result.itemId;
    result.itemType = QStringLiteral("Movie");
    result.url = QUrl::fromLocalFile(SpoolTests::fixturePath("tests/fixtures/local-thumbnail.mkv")).toString();
    result.playMethod = QStringLiteral("DirectPlay");
    return result;
}

class Surface {
public:
    Surface()
        : window(QStringLiteral("player-setup-benchmark"))
        , video(window.contentItem())
    {
        window.resize(320, 180);
        video.setSize(QSizeF(window.size()));
        QObject::connect(&window, &QQuickWindow::frameSwapped, &window, [this] { ++swaps; }, Qt::DirectConnection);
    }
    NativeAppWindow window;
    MpvVideoItem video;
    std::atomic<int> swaps { 0 };
};

QJsonObject measure(bool warm, int iteration)
{
    qputenv("SPOOL_DISABLE_IDLE_MPV", warm ? "0" : "1");
    Surface surface;
    Heartbeat heartbeat;
    const int preparationsBefore = preparedCount.load();
    const int adoptionsBefore = adoptedCount.load();
    QElapsedTimer clock;
    clock.start();
    PlayerController player(&surface.window, nullptr, nullptr, {});
    const double constructMs = clock.nsecsElapsed() / 1e6;
    player.setMpvConfigPolicy({ MpvConfigPolicy::Mode::Disabled, {} });
    surface.window.show();
    require(waitUntil([&] { return surface.swaps.load() > 0; }), "benchmark window presents its launch frame");
    heartbeat.reset();
    clock.restart();
    double readyMs = 0;
    double prepareGapMs = 0;
    int prepareTicks = 0;
    if (warm) {
        require(waitUntil([&] { return preparedCount.load() > preparationsBefore; }),
            "real background player preparation completes");
        readyMs = clock.nsecsElapsed() / 1e6;
        heartbeat.tick();
        prepareGapMs = heartbeat.maxGap;
        prepareTicks = heartbeat.ticks;
        require(!player.sessionActive() && !player.visible(), "idle preparation never starts a playback session");
    }
    const bool preparedBeforePlay = preparedCount.load() > preparationsBefore;
    heartbeat.reset();
    clock.restart();
    player.play(session(), true);
    const double playCallMs = clock.nsecsElapsed() / 1e6;
    require(waitUntil([&] { return player.fileLoaded() || !player.errorText().isEmpty(); }),
        "benchmark real player completes setup and media loading");
    const double loadedMs = clock.nsecsElapsed() / 1e6;
    require(player.fileLoaded() && player.errorText().isEmpty() && player.sessionActive(),
        "benchmark loads its real local fixture without a playback error");
    const int loadedSwaps = surface.swaps.load();
    surface.window.update();
    require(waitUntil([&] {
        if (!player.paused() || !player.embeddedVideoOutput() || surface.swaps.load() <= loadedSwaps)
            return false;
        const QImage image = surface.window.grabWindow();
        if (image.isNull())
            return false;
        const QColor center = image.pixelColor(image.width() / 2, image.height() / 2);
        return std::max({ center.red(), center.green(), center.blue() }) > 32;
    }),
        "benchmark initialized video presents a nonblack paused frame");
    const double frameMs = clock.nsecsElapsed() / 1e6;
    heartbeat.tick();
    const qint64 workerMs = preparationMs.load();
    require(preparedCount.load() > preparationsBefore && workerMs >= 0,
        "real setup reports its initialization worker duration");
    const QJsonObject result { { "mode", warm ? "prepared" : "cold" }, { "iteration", iteration },
        { "processCold", !warm && iteration == 0 }, { "constructCallMs", constructMs },
        { "preparationReadyMs", readyMs }, { "setupWorkerMs", workerMs },
        { "preparationHeartbeatMaxGapMs", prepareGapMs }, { "preparationHeartbeats", prepareTicks },
        { "playCallMs", playCallMs }, { "fileLoadedMs", loadedMs }, { "firstFrameMs", frameMs },
        { "playHeartbeatMaxGapMs", heartbeat.maxGap }, { "playHeartbeats", heartbeat.ticks },
        { "preparedAdopted", preparedBeforePlay && adoptedCount.load() > adoptionsBefore } };
    player.stop();
    player.teardownMpv();
    return result;
}

void lifecycle(const QString& replacement)
{
    qputenv("SPOOL_DISABLE_IDLE_MPV", "0");
    Surface surface;
    PlayerController player(&surface.window, nullptr, nullptr, {});
    player.setMpvConfigPolicy({ MpvConfigPolicy::Mode::Disabled, {} });
    const int before = preparedCount.load();
    surface.window.show();
    require(waitUntil([&] { return preparedCount.load() > before; }), "config lifecycle has a real prepared core");
    player.setMpvConfigPolicy({ MpvConfigPolicy::Mode::Custom, replacement });
    player.play(session(), true);
    require(waitUntil([&] { return player.fileLoaded() && player.paused() && player.volume() == 43; }),
        "changing configuration discards the prepared core and applies the replacement config");
    player.stop();
    player.teardownMpv();

    // Cancel at the consumer boundary before the asynchronous start can finish.
    // A later completion must not resurrect that request or stop its successor.
    player.play(session(), true);
    player.stop();
    require(!player.sessionActive(), "pending play can be canceled immediately");
    PlaybackSession next = session();
    next.title = QStringLiteral("successor-after-canceled-setup");
    player.play(next, true);
    require(waitUntil([&] { return player.fileLoaded() && player.paused(); }),
        "a new play succeeds after cancellation during initialization");
    require(player.title() == next.title && player.sessionActive(), "late setup cannot replace the successor session");
    player.setPaused(false);
    require(waitUntil([&] { return player.positionSeconds() > 0.2; }), "successor advances on the real media clock");
    player.stop();
    player.teardownMpv();

    player.play(session(), true);
    player.setPaused(false);
    require(waitUntil([&] { return player.fileLoaded() && !player.paused() && player.positionSeconds() > 0.2; }),
        "resume requested during setup overrides the original paused start and advances real playback");
    player.stop();
    player.teardownMpv();

    player.play(session(), false);
    player.setPaused(true);
    require(waitUntil([&] { return player.fileLoaded() && player.paused(); }),
        "pause requested during setup overrides the original playing start");
    const double pausedPosition = player.positionSeconds();
    QElapsedTimer pausedWindow;
    pausedWindow.start();
    require(
        waitUntil([&] { return pausedWindow.elapsed() >= 500; }, 2000), "pending-pause observation window completes");
    require(player.sessionActive() && player.paused() && player.positionSeconds() < pausedPosition + 0.1,
        "pending pause holds the actual media clock rather than only echoing the requested state");
    player.stop();
    player.teardownMpv();
}
} // namespace

SPOOL_TEST_MAIN("player-setup-benchmark")
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setAlphaBufferSize(0);
    QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication app(argc, argv);
    std::setlocale(LC_NUMERIC, "C");
    const QString path
        = qEnvironmentVariable("SPOOL_PLAYER_SETUP_OUT", QDir::current().filePath("player-setup-benchmark.json"));
    require(!QFile::exists(path) || QFile::remove(path), "stale benchmark output is removed before measuring");
    const QByteArray originalIdle = qgetenv("SPOOL_DISABLE_IDLE_MPV");
    previousHandler = qInstallMessageHandler(messages);
    QTemporaryDir replacement;
    require(replacement.isValid(), "benchmark replacement configuration directory opens");
    writeConfig(replacement.path(), 43);
    QJsonArray samples;
    for (int iteration = 0; iteration < 3; ++iteration) {
        samples.append(measure(false, iteration));
        samples.append(measure(true, iteration));
    }
    lifecycle(replacement.path());
    const QJsonObject report { { "schemaVersion", 1 }, { "complete", true },
        { "recordedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate) }, { "qtVersion", qVersion() },
        { "cpuArchitecture", QSysInfo::currentCpuArchitecture() }, { "qpaPlatform", QGuiApplication::platformName() },
        { "graphicsApi", "OpenGL" }, { "audioOutput", "video-only fixture; no audio stream" }, { "samples", samples },
        { "commit", qEnvironmentVariable("GITHUB_SHA") }, { "runId", qEnvironmentVariable("GITHUB_RUN_ID") },
        { "coldDefinition",
            "fresh controller, no prepared core; only first sample is process-cold; OS caches retained" },
        { "timingOrigin", "play call through real PlayerController, local demux/decode and embedded video frame" } };
    QSaveFile output(path);
    const QByteArray bytes = QJsonDocument(report).toJson();
    require(output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit(),
        "player setup benchmark retains its timing JSON");
    qInstallMessageHandler(previousHandler);
    if (originalIdle.isNull())
        qunsetenv("SPOOL_DISABLE_IDLE_MPV");
    else
        qputenv("SPOOL_DISABLE_IDLE_MPV", originalIdle);
    return EXIT_SUCCESS;
}
