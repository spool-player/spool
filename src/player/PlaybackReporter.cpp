#include "PlaybackReporter.h"

#include "../common/AsyncTask.h"

namespace JellyfinNative {

namespace {

    constexpr int kReportRetryDelayMs = 5000;
    constexpr int kMaxStopReportAttempts = 3;

} // namespace

PlaybackReporter::PlaybackReporter(PlaybackSource *api, QObject *parent)
    : QObject(parent)
    , m_api(api)
{
    m_startRetryTimer.setSingleShot(true);
    m_startRetryTimer.setInterval(kReportRetryDelayMs);
    m_progressRetryTimer.setSingleShot(true);
    m_progressRetryTimer.setInterval(kReportRetryDelayMs);

    connect(&m_startRetryTimer, &QTimer::timeout, this, &PlaybackReporter::sendStart);
    connect(&m_progressRetryTimer, &QTimer::timeout, this, &PlaybackReporter::sendProgress);
}

void PlaybackReporter::start(const PlaybackSession& session, double playbackRate, int volume, bool muted)
{
    ++m_generation;
    m_session = session;
    m_startPlaybackRate = playbackRate;
    m_startVolume = volume;
    m_startMuted = muted;
    m_active = true;
    m_startInFlight = false;
    m_startReported = false;
    m_progressInFlight = false;
    m_progressPending = false;
    m_startRetryTimer.stop();
    m_progressRetryTimer.stop();
    sendStart();
}

bool PlaybackReporter::setStreamIndexes(int audioStreamIndex, int subtitleStreamIndex)
{
    if (m_session.audioStreamIndex == audioStreamIndex && m_session.subtitleStreamIndex == subtitleStreamIndex)
        return false;
    m_session.audioStreamIndex = audioStreamIndex;
    m_session.subtitleStreamIndex = subtitleStreamIndex;
    return true;
}

void PlaybackReporter::reportProgress(qint64 positionTicks, bool paused, double playbackRate, int volume, bool muted)
{
    if (!m_active || !m_api)
        return;

    m_pendingPositionTicks = positionTicks;
    m_pendingPaused = paused;
    m_pendingPlaybackRate = playbackRate;
    m_pendingVolume = volume;
    m_pendingMuted = muted;
    m_progressPending = true;
    if (!m_progressInFlight && !m_progressRetryTimer.isActive())
        sendProgress();
}

void PlaybackReporter::stop(qint64 positionTicks, bool failed, double playbackRate)
{
    if (!m_active || !m_api)
        return;

    m_active = false;
    m_startRetryTimer.stop();
    m_progressRetryTimer.stop();
    m_progressPending = false;

    sendStop(m_session, positionTicks, failed, playbackRate, 1);
}

void PlaybackReporter::sendStop(
    const PlaybackSession& session, qint64 positionTicks, bool failed, double playbackRate, int attempt)
{
    Async::runScoped(
        this, m_api->reportPlaybackStopped(session, positionTicks, failed, playbackRate), []() {},
        [this, session, positionTicks, failed, playbackRate, attempt](const std::exception_ptr& error) {
            qWarning() << "player: playback stop report attempt" << attempt << "failed:" << exceptionMessage(error);
            emit reportFailed(QStringLiteral("playback stop"), exceptionMessage(error));
            if (attempt >= kMaxStopReportAttempts)
                return;
            QTimer::singleShot(
                kReportRetryDelayMs, this, [this, session, positionTicks, failed, playbackRate, attempt]() {
                    sendStop(session, positionTicks, failed, playbackRate, attempt + 1);
                });
        },
        "playback stop report");
}

void PlaybackReporter::sendStart()
{
    if (!m_active || !m_api || m_startInFlight || m_startReported)
        return;

    m_startInFlight = true;
    const PlaybackSession session = m_session;
    const quint64 generation = m_generation;
    Async::runScoped(
        this, m_api->reportPlaybackStart(session, m_startPlaybackRate, m_startVolume, m_startMuted),
        [this, generation]() {
            if (generation != m_generation)
                return;
            m_startInFlight = false;
            m_startReported = true;
        },
        [this, generation](const std::exception_ptr& error) {
            if (generation != m_generation)
                return;
            m_startInFlight = false;
            if (m_active)
                m_startRetryTimer.start();
            qWarning() << "player: playback start report failed:" << exceptionMessage(error);
            emit reportFailed(QStringLiteral("playback start"), exceptionMessage(error));
        },
        "playback start report");
}

void PlaybackReporter::sendProgress()
{
    if (!m_active || !m_api || m_progressInFlight || !m_progressPending)
        return;

    m_progressInFlight = true;
    m_progressPending = false;
    const PlaybackSession session = m_session;
    const qint64 positionTicks = m_pendingPositionTicks;
    const bool paused = m_pendingPaused;
    const double playbackRate = m_pendingPlaybackRate;
    const int volume = m_pendingVolume;
    const bool muted = m_pendingMuted;
    const quint64 generation = m_generation;
    Async::runScoped(
        this, m_api->reportPlaybackProgress(session, positionTicks, paused, playbackRate, volume, muted),
        [this, generation]() {
            if (generation != m_generation)
                return;
            m_progressInFlight = false;
            if (m_progressPending)
                sendProgress();
        },
        [this, generation](const std::exception_ptr& error) {
            if (generation != m_generation)
                return;
            m_progressInFlight = false;
            m_progressPending = true;
            if (m_active)
                m_progressRetryTimer.start();
            qWarning() << "player: playback progress report failed:" << exceptionMessage(error);
            emit reportFailed(QStringLiteral("playback progress"), exceptionMessage(error));
        },
        "playback progress report");
}

} // namespace JellyfinNative
