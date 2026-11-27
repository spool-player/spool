#include "player/PlayerController.h"
#include "TestMain.h"
#include "TestRequire.h"
#include "app/UserItemStateController.h"
#include "platform/NativeAppWindow.h"
#include "player/MpvVideoItem.h"
#include "providers/local/LocalProvider.h"

#include <QCoroFuture>
#include <QCoroTask>
#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QPromise>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <utility>

extern "C" {
#include <mpv/client.h>
}

namespace Spool {
struct PlayerControllerTestAccess {
    static void runCacheContract(NativeAppWindow& window)
    {
        using SpoolTests::require;
        constexpr qint64 mib = 1024LL * 1024;
        PlayerController player(&window, nullptr, nullptr, {});
        player.m_idleStreamingMemoryTimer.stop();
        player.m_idleMpvPreparationEnabled = false;
        player.m_forwardCacheSizeMiB = 32;
        player.m_memoryProbe = [] { return PlatformStreamingMemoryState {}; };
        const auto waitPrepared = [&] {
            QElapsedTimer deadline;
            deadline.start();
            while (!player.m_idleMpvHandle && deadline.elapsed() < 10000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(1);
            }
            require(player.m_idleMpvHandle, "preparation hands off its owned idle core");
        };
        const auto readPair = [&](StreamingCacheTarget expected) {
            int64_t forward = 0, back = 0;
            auto *core = player.m_mpvLifecycle.handle();
            require(core && mpv_get_property(core, "demuxer-max-bytes", MPV_FORMAT_INT64, &forward) >= 0
                    && mpv_get_property(core, "demuxer-max-back-bytes", MPV_FORMAT_INT64, &back) >= 0
                    && forward == expected.forwardBytes && back == expected.backBytes,
                "actual adopted core limits match the latest pair before media load");
        };
        player.applyDemuxerCacheTarget({ 512 * mib, 128 * mib }, false);
        player.prepareMpvAsync(false, false);
        player.applyDemuxerCacheTarget({ 32 * mib, 16 * mib }, false);
        waitPrepared();
        require(player.ensureMpv(false, false), "a pressure change during preparation survives adoption");
        readPair({ 32 * mib, 16 * mib });
        player.teardownMpv();
        player.prepareMpvAsync(false, false);
        waitPrepared();
        auto *readyCore = player.m_idleMpvHandle;
        player.applyDemuxerCacheTarget({ 64 * mib, 32 * mib }, false);
        require(player.ensureMpv(false, false) && player.m_mpvLifecycle.handle() == readyCore,
            "a changed floor refreshes the same ready speculative core without reinitialization");
        readPair({ 64 * mib, 32 * mib });
        player.teardownMpv();
        player.prepareMpvAsync(false, false);
        player.destroyIdleMpv("test cancellation");
        QElapsedTimer cancellation;
        cancellation.start();
        while (cancellation.elapsed() < 100) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        require(!player.m_idleMpvHandle && !player.m_mpvPreparation,
            "cancelled preparation cannot publish an abandoned idle core");
        auto *handle = mpv_create();
        require(handle && mpv_set_option_string(handle, "config", "no") >= 0
                && mpv_set_option_string(handle, "vo", "null") >= 0 && mpv_set_option_string(handle, "ao", "null") >= 0
                && mpv_initialize(handle) >= 0,
            "cache application fixture initializes an idle core");
        player.applyDemuxerCacheTarget({ 512 * mib, 128 * mib }, false);
        require(player.applyInitialDemuxerCacheTarget(handle), "initial limits apply before core adoption");
        player.applyDemuxerCacheTarget({ 32 * mib, 16 * mib }, false);
        require(player.applyInitialDemuxerCacheTarget(handle), "adoption refreshes both limits after idle pressure");
        int64_t forward = 0, back = 0;
        require(mpv_get_property(handle, "demuxer-max-bytes", MPV_FORMAT_INT64, &forward) >= 0
                && mpv_get_property(handle, "demuxer-max-back-bytes", MPV_FORMAT_INT64, &back) >= 0
                && forward == 32 * mib && back == 16 * mib,
            "the adopted core reports the current pair without a later memory sample");
        require(player.m_mpvLifecycle.adopt(handle, [](mpv_event *) { }), "test adopts cache core");
        int writes = 0;
        bool rejectBack = true;
        player.m_cachePropertyWriter = [&](mpv_handle *, quint64, const char *name, qint64, bool async) {
            ++writes;
            require(async, "live cache writes stay asynchronous");
            return rejectBack && QByteArray(name) == "demuxer-max-back-bytes" ? MPV_ERROR_PROPERTY_ERROR : 0;
        };
        player.applyDemuxerCacheTarget({ 64 * mib, 32 * mib }, true);
        const quint64 failedSerial = player.m_demuxerCacheSerial;
        player.completeDemuxerCacheWrite(failedSerial, 0);
        require(!player.m_appliedDemuxerTarget && player.m_demuxerCacheReplies == 0,
            "one failed property leaves the entire pair dirty");
        rejectBack = false;
        player.applyDemuxerCacheTarget({ 64 * mib, 32 * mib }, true);
        require(
            writes == 4 && player.m_demuxerCacheReplies == 2, "an unchanged target retries after an enqueue failure");
        player.completeDemuxerCacheWrite(failedSerial, 0);
        require(player.m_demuxerCacheReplies == 2, "stale completions cannot acknowledge a replacement update");
        player.completeDemuxerCacheWrite(player.m_demuxerCacheSerial, 0);
        require(!player.m_appliedDemuxerTarget, "one completion cannot acknowledge both cache limits");
        player.completeDemuxerCacheWrite(player.m_demuxerCacheSerial, MPV_ERROR_PROPERTY_ERROR);
        require(!player.m_appliedDemuxerTarget, "a completion error stays dirty");
        player.applyDemuxerCacheTarget({ 64 * mib, 32 * mib }, true);
        player.completeDemuxerCacheWrite(player.m_demuxerCacheSerial, 0);
        player.completeDemuxerCacheWrite(player.m_demuxerCacheSerial, 0);
        require(player.m_appliedDemuxerTarget == player.m_activeDemuxerTarget,
            "only both successful completions acknowledge the current pair");
        player.m_cachePropertyWriter = {};
        player.teardownMpv();
        require(!player.m_streamingMemoryTimer.isActive() && !player.m_appliedDemuxerTarget,
            "teardown retires pressure polling and per-core applied state");
    }
};
}

using namespace Spool;

namespace {
using SpoolTests::require;

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

// Only delivery timing is controlled; every report and state mutation still
// reaches the real local provider and its durable state file.
class DelayedPlaybackSource final : public PlaybackSource {
public:
    explicit DelayedPlaybackSource(PlaybackSource *source, bool start, bool progress, bool firstStopOnly = false)
        : m_source(source)
        , m_holdStart(start)
        , m_holdProgress(progress)
        , m_firstStopOnly(firstStopOnly)
    {
        startGate.start();
        progressGate.start();
        stopGate.start();
    }

    QByteArray mediaRequestHeaders() const override
    {
        return m_source->mediaRequestHeaders();
    }
    QUrl mediaOrigin() const override
    {
        return m_source->mediaOrigin();
    }
    bool signedIn() const override
    {
        return m_source->signedIn();
    }
    QCoro::Task<PlaybackSession> resolvePlayback(MovieItem item, bool force) override
    {
        co_return co_await m_source->resolvePlayback(std::move(item), force);
    }
    QCoro::Task<std::vector<MediaSegment>> fetchMediaSegments(QString id) override
    {
        co_return co_await m_source->fetchMediaSegments(std::move(id));
    }
    QCoro::Task<std::vector<MovieItem>> fetchSeriesEpisodes(QString id) override
    {
        co_return co_await m_source->fetchSeriesEpisodes(std::move(id));
    }
    QCoro::Task<void> reportPlaybackStart(PlaybackSession session, double rate, int volume, bool muted) override
    {
        startEntered = true;
        if (m_holdStart)
            co_await startGate.future();
        co_await m_source->reportPlaybackStart(std::move(session), rate, volume, muted);
    }
    QCoro::Task<void> reportPlaybackProgress(
        PlaybackSession session, qint64 ticks, bool paused, double rate, int volume, bool muted) override
    {
        progressEntered = true;
        if (m_holdProgress)
            co_await progressGate.future();
        co_await m_source->reportPlaybackProgress(std::move(session), ticks, paused, rate, volume, muted);
    }
    QCoro::Task<void> reportPlaybackStopped(PlaybackSession session, qint64 ticks, bool failed, double rate) override
    {
        ++stopsEntered;
        if (!m_firstStopOnly || stopsEntered == 1)
            co_await stopGate.future();
        co_await m_source->reportPlaybackStopped(std::move(session), ticks, failed, rate);
    }

    QPromise<void> startGate;
    QPromise<void> progressGate;
    QPromise<void> stopGate;
    bool startEntered = false;
    bool progressEntered = false;
    int stopsEntered = 0;

private:
    PlaybackSource *m_source;
    bool m_holdStart;
    bool m_holdProgress;
    bool m_firstStopOnly;
};

void watchedStopPolicy(const QString& directory, NativeAppWindow& window)
{
    QTemporaryDir stateDirectory;
    require(stateDirectory.isValid(), "durable watched fixture directory opens");
    const PlaybackSession media = audioSession(directory, QStringLiteral("watched-policy"), 20);
    const QVariantList inventory { QVariantMap { { QStringLiteral("path"), QUrl(media.url).toLocalFile() },
        { QStringLiteral("metadata"),
            QVariantMap { { QStringLiteral("title"), media.title }, { QStringLiteral("type"), QStringLiteral("Audio") },
                { QStringLiteral("runtimeTicks"), media.runtimeTicks } } } } };
    LocalProvider provider(QStringLiteral("watched-test"), {}, nullptr, inventory, stateDirectory.path());
    provider.scan();
    const auto items = QCoro::waitFor(provider.fetchLatestItems());
    require(items.size() == 1, "real local provider supplies the watched-policy item");
    const MovieItem item = items.front();
    const auto details = [&] { return QCoro::waitFor(provider.fetchItemDetails(item.id)); };
    UserItemStateController itemState(&provider, nullptr, nullptr, nullptr, nullptr);
    PlayerController player(&window, provider.playback(), nullptr, {});
    player.setMpvConfigPolicy({ MpvConfigPolicy::Mode::Custom, directory });
    bool watched = false;
    bool reachedEnd = false;
    qint64 stoppedTicks = 0;
    quint64 stoppedReportId = 0;
    int stops = 0;
    const auto stopped = [&](const QString& id, qint64 ticks, bool isWatched, bool ended, quint64 reportId) {
        ++stops;
        watched = isWatched;
        reachedEnd = ended;
        stoppedTicks = ticks;
        stoppedReportId = reportId;
        itemState.recordPlaybackStopped(item, id, ticks, isWatched, {}, reportId);
    };
    QObject::connect(&player, &PlayerController::playbackStopped, &itemState, stopped);
    QObject::connect(&player, &PlayerController::watchedPersistenceRequested, &itemState,
        &UserItemStateController::persistPlaybackWatched);
    const auto startAt = [&](PlayerController& target, double seconds) {
        PlaybackSession session = QCoro::waitFor(provider.playback()->resolvePlayback(item, false));
        session.startTimeTicks = qRound64(seconds * 10'000'000.0);
        target.play(session, true);
        waitUntil([&] { return target.fileLoaded() && target.paused() && !target.seeking(); },
            "paused threshold fixture loads and settles");
        require(std::abs(target.positionSeconds() - seconds) < 0.02,
            "paused fixture starts near its target; the stop receipt determines the actual threshold input");
    };

    struct Case {
        int percent;
        double seconds;
        std::optional<bool> watched;
    };
    for (const Case test : { Case { 90, 17.75, false }, Case { 90, 18.0, std::nullopt }, Case { 90, 18.25, true },
             Case { 95, 18.0, false }, Case { 50, 10.0, std::nullopt }, Case { 50, 10.25, true },
             Case { 100, 19.5, false } }) {
        itemState.setPlayed(item.id, false);
        player.setWatchedThresholdPercent(test.percent);
        startAt(player, test.seconds);
        const double observedDuration = player.durationSeconds();
        const bool wasSeeking = player.seeking();
        player.stop();
        const qint64 observedRuntimeTicks = qRound64(observedDuration * 10'000'000.0);
        const bool observedQualifies
            = observedRuntimeTicks > 0 && stoppedTicks * 100 >= observedRuntimeTicks * test.percent;
        const bool intendedSide = !test.watched.has_value() || observedQualifies == *test.watched;
        const bool classificationMatches = !player.sessionActive() && watched == observedQualifies && !reachedEnd;
        if (!intendedSide || !classificationMatches) {
            std::cerr << std::setprecision(17) << "watched-stop receipt: percent=" << test.percent
                      << " requestedSeconds=" << test.seconds << " stoppedTicks=" << stoppedTicks
                      << " observedDuration=" << observedDuration << " seekingBeforeStop=" << wasSeeking
                      << " reportId=" << stoppedReportId << " watched=" << watched << " reachedEnd=" << reachedEnd
                      << " observedQualifies=" << observedQualifies << '\n';
        }
        require(observedRuntimeTicks == media.runtimeTicks && intendedSide,
            "real native stop input must have the known WAV duration and lie on the intended threshold side");
        require(classificationMatches,
            "configured explicit stop follows its observed native position without a natural end or successor");
        waitUntil(
            [&] { return details().played == observedQualifies; }, "stop policy reaches the durable item-state sink");
        processFor(50);
        const MovieItem stored = details();
        require(stored.resumeTicks == (observedQualifies ? 0 : stoppedTicks),
            "watched clears resume only after the stopped report; unwatched near-end progress stays honest");
        LocalProvider restored(QStringLiteral("restored-watched-test"), {}, nullptr, inventory, stateDirectory.path());
        restored.scan();
        const MovieItem reloaded = QCoro::waitFor(restored.fetchItemDetails(item.id));
        require(reloaded.played == observedQualifies && reloaded.resumeTicks == stored.resumeTicks,
            "watched and resume state survive a fresh provider, not just an in-memory echo");
        player.teardownMpv();
    }

    itemState.setPlayed(item.id, false);
    player.setWatchedThresholdPercent(90);
    startAt(player, 18.25);
    player.stopWithReason(QStringLiteral("account-identity-changed"));
    require(!watched && !reachedEnd && !details().played, "internal interruption is not an explicit watched stop");
    player.teardownMpv();
    startAt(player, 0.0);
    player.seek(18.0);
    player.stop();
    require(!watched && !reachedEnd && !details().played, "an unconfirmed seek target cannot mark playback watched");
    player.teardownMpv();
    PlaybackSession missing = QCoro::waitFor(provider.playback()->resolvePlayback(item, false));
    missing.url = QUrl::fromLocalFile(directory + QStringLiteral("/missing.wav")).toString();
    missing.startTimeTicks = 180'000'000;
    const int previousStops = stops;
    player.play(missing, true);
    waitUntil(
        [&] { return stops > previousStops && !player.sessionActive(); }, "failed media produces a stopped event");
    require(!watched && !reachedEnd && !details().played, "failed loading at a qualifying seed never marks watched");
    player.teardownMpv();

    // A successor can replace the reporter's current session while the old
    // progress is in flight. Its eventual stop must retain the old report ID.
    {
        DelayedPlaybackSource delayed(provider.playback(), true, true);
        PlayerController ordered(&window, &delayed, nullptr, {});
        ordered.setMpvConfigPolicy({ MpvConfigPolicy::Mode::Custom, directory });
        QObject::connect(&ordered, &PlayerController::playbackStopped, &itemState, stopped);
        QObject::connect(&ordered, &PlayerController::watchedPersistenceRequested, &itemState,
            &UserItemStateController::persistPlaybackWatched);
        startAt(ordered, 18.25);
        ordered.setVolume(77);
        require(delayed.startEntered && !delayed.progressEntered, "progress waits for the outstanding start report");
        delayed.startGate.finish();
        waitUntil([&] { return delayed.progressEntered; }, "the real progress report enters its delivery barrier");
        ordered.stop();
        require(watched && !reachedEnd && delayed.stopsEntered == 0 && !details().played,
            "watched is immediate locally but stop and durable played wait for outstanding progress");
        startAt(ordered, 0.0);
        delayed.progressGate.finish();
        waitUntil([&] { return delayed.stopsEntered == 1; }, "old-session stop survives a new reporting session");
        require(!details().played, "durable watched still waits for the final stopped report");
        delayed.stopGate.finish();
        waitUntil([&] { return details().played; }, "drained stop persists watched through the existing sink");
        require(details().resumeTicks == 0, "late stop cannot restore resume after the durable watched mutation");
        ordered.stopWithReason(QStringLiteral("account-identity-changed"));
        processFor(50);
        require(details().played, "a later resume-only report does not silently undo already-played state");
        ordered.teardownMpv();
    }
    itemState.setPlayed(item.id, false);
    {
        DelayedPlaybackSource delayed(provider.playback(), false, false);
        PlayerController cancelled(&window, &delayed, nullptr, {});
        cancelled.setMpvConfigPolicy({ MpvConfigPolicy::Mode::Custom, directory });
        QObject::connect(&cancelled, &PlayerController::playbackStopped, &itemState, stopped);
        QObject::connect(&cancelled, &PlayerController::watchedPersistenceRequested, &itemState,
            &UserItemStateController::persistPlaybackWatched);
        startAt(cancelled, 18.25);
        cancelled.stop();
        require(
            watched && delayed.stopsEntered == 1 && !details().played, "watched mutation is pending on stop receipt");
        itemState.setPlayed(item.id, false);
        delayed.stopGate.finish();
        processFor(100);
        require(!details().played, "explicit Mark unwatched revokes a completion still waiting for its stopped report");
        cancelled.teardownMpv();
    }
    itemState.setPlayed(item.id, false);
    {
        // A's stopped report remains held; later reports are free to overtake
        // unless the production reporter serializes same-item drainage.
        DelayedPlaybackSource delayed(provider.playback(), false, false, true);
        PlayerController repeated(&window, &delayed, nullptr, {});
        repeated.setMpvConfigPolicy({ MpvConfigPolicy::Mode::Custom, directory });
        QObject::connect(&repeated, &PlayerController::playbackStopped, &itemState, stopped);
        QObject::connect(&repeated, &PlayerController::watchedPersistenceRequested, &itemState,
            &UserItemStateController::persistPlaybackWatched);
        startAt(repeated, 18.25);
        repeated.stop();
        require(watched && delayed.stopsEntered == 1, "first repeated-item watched stop enters its held report");
        startAt(repeated, 18.5);
        repeated.stop();
        require(watched && !reachedEnd, "second repeated-item stop also qualifies without natural advancement");
        processFor(100);
        delayed.stopGate.finish();
        waitUntil([&] { return delayed.stopsEntered == 2 && details().played; },
            "both same-item stopped reports drain and persist the newest watched operation");
        processFor(100);
        require(details().resumeTicks == 0, "older same-item stopped report cannot resurrect cleared watched progress");
        LocalProvider restored(QStringLiteral("restored-repeat-test"), {}, nullptr, inventory, stateDirectory.path());
        restored.scan();
        const MovieItem stored = QCoro::waitFor(restored.fetchItemDetails(item.id));
        require(stored.played && stored.resumeTicks == 0,
            "the final repeated-item watched and cleared resume state survive a fresh provider");
        repeated.teardownMpv();
    }
}

void transportLoadLifecycle(const QString& directory, NativeAppWindow& window)
{
    PlayerController player(&window, nullptr, nullptr, {});
    player.setMpvConfigPolicy({ MpvConfigPolicy::Mode::Custom, directory });
    PlaybackSession rejected = audioSession(directory, QStringLiteral("rejected-policy"), 8);
    rejected.sourceBitrate = 1'000'000'000'001LL;
    player.play(rejected);
    waitUntil([&] { return !player.sessionActive() && !player.errorText().isEmpty(); },
        "a rejected transport configure ends the pending load");
    require(!player.fileLoaded() && player.errorText().contains(QStringLiteral("streaming policy")),
        "failed policy installation cannot load a playable file");

    const PlaybackSession replacement = audioSession(directory, QStringLiteral("replacement-policy"), 8);
    player.play(rejected);
    player.play(replacement);
    waitUntil([&] { return player.fileLoaded() && player.positionSeconds() > 0.1; },
        "a current successful configure releases the replacement load");
    require(player.title() == replacement.title && player.errorText().isEmpty(),
        "a superseded configure failure cannot reject a replacement playback");
    player.stop();

    player.play(replacement);
    player.stop();
    processFor(500);
    require(!player.sessionActive() && !player.fileLoaded(),
        "stopping a pending configure cannot be undone by its acknowledgement");
    player.teardownMpv();
}

#if !defined(Q_OS_ANDROID) && !defined(SPOOL_APPLE_MOBILE)
void localPlaylistLifecycle(const QString& directory, NativeAppWindow& window)
{
    const PlaybackSession first = audioSession(directory, QStringLiteral("native-first"), 1);
    const PlaybackSession tail = audioSession(directory, QStringLiteral("native-tail"), 1);
    QFile playlist(directory + QStringLiteral("/native-redirect.data"));
    require(playlist.open(QIODevice::WriteOnly), "redirect playlist fixture opens");
    const QByteArray contents = QByteArrayLiteral("#EXTM3U\n") + QUrl(first.url).toLocalFile().toUtf8() + '\n';
    require(playlist.write(contents) == contents.size(), "redirect playlist fixture writes");
    playlist.close();
    PlayerController player(&window, nullptr, nullptr, {});
    player.setMpvConfigPolicy({ MpvConfigPolicy::Mode::Custom, directory });
    QStringList endedIds;
    QObject::connect(
        &player, &PlayerController::localEntryEnded, &player, [&](const QString& id, qint64 ticks, bool completed) {
            require(completed && ticks > 0 && player.sessionActive(),
                "native entry end is completed without tearing down the whole playlist session");
            endedIds.append(id);
        });
    int providerStops = 0;
    QObject::connect(&player, &PlayerController::playbackStopped, &player, [&] { ++providerStops; });
    player.playLocalFiles({ QUrl::fromLocalFile(playlist.fileName()), QUrl(tail.url) });
    waitUntil([&] { return endedIds.size() == 2 && !player.sessionActive(); },
        "mpv redirect expands and plays the following file before final idle teardown");
    require(endedIds[0] != endedIds[1] && providerStops == 0,
        "native entries retain distinct mpv identities and do not trigger provider queue/autoplay");
    player.teardownMpv();
    QFile empty(directory + QStringLiteral("/native-empty.m3u"));
    require(empty.open(QIODevice::WriteOnly), "empty native playlist fixture opens");
    require(empty.write("#EXTM3U\n") == 8, "empty native playlist fixture writes");
    empty.close();
    player.playLocalFiles({ QUrl::fromLocalFile(empty.fileName()) });
    waitUntil([&] { return !player.sessionActive() && !player.errorText().isEmpty(); },
        "a real zero-entry playlist is rejected with an error instead of remaining active and preparing");
    player.teardownMpv();
}
#endif
} // namespace

SPOOL_TEST_MAIN("player-controller")
{
#if !defined(Q_OS_ANDROID) && !defined(SPOOL_APPLE_MOBILE)
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setAlphaBufferSize(0);
    QSurfaceFormat::setDefaultFormat(format);
#endif
    QGuiApplication app(argc, argv);
    std::setlocale(LC_NUMERIC, "C");
    QTemporaryDir directory;
    require(directory.isValid(), "player fixture directory opens");
    QFile config(directory.filePath(QStringLiteral("mpv.conf")));
    require(config.open(QIODevice::WriteOnly) && config.write("ao=null\n") == 8, "silent mpv configuration writes");
    config.close();

    NativeAppWindow window(QStringLiteral("player-controller-test"));
    PlayerControllerTestAccess::runCacheContract(window);
#if !defined(Q_OS_ANDROID) && !defined(SPOOL_APPLE_MOBILE)
    window.resize(320, 180);
    MpvVideoItem video(window.contentItem());
    video.setSize(QSizeF(window.size()));
    QObject::connect(&window, &QWindow::widthChanged, &video, [&] { video.setWidth(window.width()); });
    QObject::connect(&window, &QWindow::heightChanged, &video, [&] { video.setHeight(window.height()); });
    std::atomic<int> swaps { 0 };
    QObject::connect(&window, &QQuickWindow::frameSwapped, &window, [&] { ++swaps; }, Qt::DirectConnection);
    window.show();
    waitUntil([&] { return swaps.load() > 0; }, "native player fixture presents its real first frame");
#endif
    PlayerController player(&window, nullptr, nullptr, {});
    player.setMpvConfigPolicy({ MpvConfigPolicy::Mode::Custom, directory.path() });
    const PlaybackSession first = audioSession(directory.path(), QStringLiteral("episode-1"), 1);
    const PlaybackSession second = audioSession(directory.path(), QStringLiteral("episode-2"), 8);
    int completions = 0;
    int manualStops = 0;
    QObject::connect(&player, &PlayerController::playbackStopped, &player,
        [&](const QString& itemId, qint64 ticks, bool watched, bool reachedEnd, quint64) {
            if (!reachedEnd) {
                ++manualStops;
                return;
            }
            ++completions;
            require(watched, "natural successful EOF marks watched");
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
    player.setPaused(false);
    player.setPaused(true);
    waitUntil([&] { return player.paused() && player.pauseCommandState() == QStringLiteral("paused"); },
        "readback of an older resume cannot replace the latest pause request");
    player.stopWithReason(QStringLiteral("test-manual-stop"), true);
    require(!player.sessionActive() && completions == 1 && manualStops == 1, "manual stop never advances");

    player.play(second, true);
    waitUntil([&] { return player.fileLoaded(); }, "group-style paused startup loads");
    processFor(300);
    require(player.paused() && player.positionSeconds() < 0.2, "paused startup waits for explicit unpause");
    player.setPaused(false);
    waitUntil([&] { return !player.paused() && player.positionSeconds() > 0.2; }, "explicit unpause advances playback");
    player.setPaused(true);
    require(!player.paused(), "the current core has a pause request in flight before the restart");
    player.play(second, false, true);
    waitUntil([&] { return player.fileLoaded(); }, "same-item renderer restart loads its replacement");
    require(player.paused(), "same-item renderer restart carries a pause that the previous core had not acknowledged");
    player.stop();
    player.teardownMpv();
    watchedStopPolicy(directory.path(), window);
    transportLoadLifecycle(directory.path(), window);
#if !defined(Q_OS_ANDROID) && !defined(SPOOL_APPLE_MOBILE)
    localPlaylistLifecycle(directory.path(), window);
#endif
    return EXIT_SUCCESS;
}
