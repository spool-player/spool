#include "player/PlayerController.h"
#include "TestMain.h"
#include "platform/NativeAppWindow.h"

#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QThread>

#include <clocale>
#include <cstdlib>
#include <functional>
#include <iostream>

using namespace Spool;

namespace {
void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
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

void processFor(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
}

PlaybackSession audioSession(const QString& directory, const QString& id, int seconds)
{
    const QString path = directory + QLatin1Char('/') + id + QStringLiteral(".wav");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "audio fixture opens");
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 sampleRate = 8000;
    const quint32 bytes = sampleRate * seconds * 2;
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + bytes);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(1) << sampleRate << quint32(sampleRate * 2) << quint16(2)
           << quint16(16);
    stream.writeRawData("data", 4);
    stream << bytes;
    const QByteArray silence(bytes, '\0');
    require(stream.writeRawData(silence.constData(), silence.size()) == silence.size(), "audio fixture writes");
    file.close();
    PlaybackSession session;
    session.itemId = id;
    session.title = id;
    session.itemType = QStringLiteral("Audio");
    session.url = QUrl::fromLocalFile(path).toString();
    session.runtimeTicks = qint64(seconds) * 10'000'000;
    session.playMethod = QStringLiteral("DirectPlay");
    return session;
}
} // namespace

SPOOL_TEST_MAIN("player-controller")
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication app(argc, argv);
    std::setlocale(LC_NUMERIC, "C");
    QTemporaryDir directory;
    require(directory.isValid(), "player fixture directory opens");
    QFile config(directory.filePath(QStringLiteral("mpv.conf")));
    require(config.open(QIODevice::WriteOnly) && config.write("ao=null\n") == 8, "silent mpv configuration writes");
    config.close();

    NativeAppWindow window(QStringLiteral("player-controller-test"));
    PlayerController player(&window, nullptr, nullptr, {});
    player.setMpvConfigPolicy({ MpvConfigPolicy::Mode::Custom, directory.path() });
    const PlaybackSession first = audioSession(directory.path(), QStringLiteral("episode-1"), 1);
    const PlaybackSession second = audioSession(directory.path(), QStringLiteral("episode-2"), 8);
    int completions = 0;
    int manualStops = 0;
    QObject::connect(
        &player, &PlayerController::playbackStopped, &player, [&](const QString& itemId, qint64 ticks, bool completed) {
            if (!completed) {
                ++manualStops;
                return;
            }
            ++completions;
            require(itemId == first.itemId && ticks == first.runtimeTicks, "EOF reports the ending episode");
            // Production callers may resolve a cached successor synchronously.
            player.play(second);
        });

    player.play(first);
    waitUntil([&] { return completions == 1 && player.fileLoaded(); }, "real EOF starts the successor");
    require(player.title() == second.title && !player.paused(), "successor starts unpaused");
    processFor(1600);
    require(player.sessionActive() && player.fileLoaded() && player.title() == second.title && !player.paused()
            && player.positionSeconds() > 1.0 && manualStops == 0,
        "ending-core events and deferred teardown cannot stop or pause the advancing successor");

    player.setPaused(true);
    waitUntil([&] { return player.paused(); }, "manual pause is acknowledged");
    const double heldPosition = player.positionSeconds();
    processFor(400);
    require(player.paused() && player.positionSeconds() < heldPosition + 0.2, "intentional pause stays held");
    player.stopWithReason(QStringLiteral("test-manual-stop"));
    require(!player.sessionActive() && completions == 1 && manualStops == 1, "manual stop never advances");

    player.play(second, true);
    waitUntil([&] { return player.fileLoaded(); }, "group-style paused startup loads");
    processFor(300);
    require(player.paused() && player.positionSeconds() < 0.2, "paused startup waits for explicit unpause");
    player.setPaused(false);
    waitUntil([&] { return !player.paused() && player.positionSeconds() > 0.2; }, "explicit unpause advances playback");
    player.stop();
    player.teardownMpv();
    return EXIT_SUCCESS;
}
