#include "PlaybackReporter.h"

#include "../common/AsyncTask.h"

namespace Spool {

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
    m_report = std::make_shared<Report>();
    m_report->session = session;
    m_report->id = ++m_nextReportId;
    m_startPlaybackRate = playbackRate;
    m_startVolume = volume;
    m_startMuted = muted;
    m_progressPending = false;
    m_startRetryTimer.stop();
    m_progressRetryTimer.stop();
    sendStart();
}

bool PlaybackReporter::setStreamIndexes(int audioStreamIndex, int subtitleStreamIndex)
{
    if (!m_report || !m_report->active
        || (m_report->session.audioStreamIndex == audioStreamIndex
            && m_report->session.subtitleStreamIndex == subtitleStreamIndex)) {
        return false;
    }
    m_report->session.audioStreamIndex = audioStreamIndex;
    m_report->session.subtitleStreamIndex = subtitleStreamIndex;
    return true;
}

void PlaybackReporter::reportProgress(qint64 positionTicks, bool paused, double playbackRate, int volume, bool muted)
{
    if (!m_report || !m_report->active || !m_api)
        return;

    m_pendingPositionTicks = positionTicks;
    m_pendingPaused = paused;
    m_pendingPlaybackRate = playbackRate;
    m_pendingVolume = volume;
    m_pendingMuted = muted;
    m_progressPending = true;
    if (!m_report->progressInFlight && !m_progressRetryTimer.isActive())
        sendProgress();
}

quint64 PlaybackReporter::stop(qint64 positionTicks, bool failed, double playbackRate, bool watched)
{
    const ReportPtr report = m_report;
    if (!report || !report->active)
        return 0;

    report->active = false;
    report->stopPending = true;
    report->stopPositionTicks = positionTicks;
    report->stopFailed = failed;
    report->stopPlaybackRate = playbackRate;
    report->watched = watched;
    const ReportPtr predecessor = m_stopTails.value(report->session.itemId);
    if (predecessor) {
        report->predecessor = predecessor;
        predecessor->successor = report;
    }
    m_stopTails.insert(report->session.itemId, report);
    m_startRetryTimer.stop();
    m_progressRetryTimer.stop();
    m_progressPending = false;
    sendStopIfReady(report);
    return report->id;
}

void PlaybackReporter::sendStopIfReady(const ReportPtr& report)
{
    if (!report->stopPending || report->startInFlight || report->progressInFlight
        || (report->predecessor && !report->predecessor->stopFinished))
        return;
    report->stopPending = false;
    if (!m_api) {
        finishStop(report);
        return;
    }
    sendStop(report, 1);
}

void PlaybackReporter::sendStop(const ReportPtr& report, int attempt)
{
    Async::runScoped(
        this,
        m_api->reportPlaybackStopped(
            report->session, report->stopPositionTicks, report->stopFailed, report->stopPlaybackRate),
        [this, report]() { finishStop(report); },
        [this, report, attempt](const std::exception_ptr& error) {
            qWarning() << "player: playback stop report attempt" << attempt << "failed:" << exceptionMessage(error);
            emit reportFailed(QStringLiteral("playback stop"), exceptionMessage(error));
            if (attempt >= kMaxStopReportAttempts) {
                finishStop(report);
                return;
            }
            QTimer::singleShot(kReportRetryDelayMs, this, [this, report, attempt]() { sendStop(report, attempt + 1); });
        },
        "playback stop report");
}

void PlaybackReporter::finishStop(const ReportPtr& report)
{
    report->stopFinished = true;
    report->predecessor.reset();
    if (m_stopTails.value(report->session.itemId) == report)
        m_stopTails.remove(report->session.itemId);
    if (report->watched)
        emit watchedPersistenceRequested(report->session.itemId, report->id);
    if (const ReportPtr successor = report->successor.lock())
        sendStopIfReady(successor);
}

void PlaybackReporter::sendStart()
{
    const ReportPtr report = m_report;
    if (!report || !report->active || !m_api || report->startInFlight || report->startReported)
        return;

    report->startInFlight = true;
    Async::runScoped(
        this, m_api->reportPlaybackStart(report->session, m_startPlaybackRate, m_startVolume, m_startMuted),
        [this, report]() {
            report->startInFlight = false;
            report->startReported = true;
            sendStopIfReady(report);
            if (report == m_report && report->active)
                sendProgress();
        },
        [this, report](const std::exception_ptr& error) {
            report->startInFlight = false;
            if (report == m_report && report->active)
                m_startRetryTimer.start();
            qWarning() << "player: playback start report failed:" << exceptionMessage(error);
            emit reportFailed(QStringLiteral("playback start"), exceptionMessage(error));
            sendStopIfReady(report);
        },
        "playback start report");
}

void PlaybackReporter::sendProgress()
{
    const ReportPtr report = m_report;
    if (!report || !report->active || !m_api || !report->startReported || report->progressInFlight
        || !m_progressPending)
        return;

    report->progressInFlight = true;
    m_progressPending = false;
    const qint64 positionTicks = m_pendingPositionTicks;
    const bool paused = m_pendingPaused;
    const double playbackRate = m_pendingPlaybackRate;
    const int volume = m_pendingVolume;
    const bool muted = m_pendingMuted;
    Async::runScoped(
        this, m_api->reportPlaybackProgress(report->session, positionTicks, paused, playbackRate, volume, muted),
        [this, report]() {
            report->progressInFlight = false;
            sendStopIfReady(report);
            if (report == m_report && report->active)
                sendProgress();
        },
        [this, report](const std::exception_ptr& error) {
            report->progressInFlight = false;
            if (report == m_report && report->active) {
                m_progressPending = true;
                m_progressRetryTimer.start();
            }
            qWarning() << "player: playback progress report failed:" << exceptionMessage(error);
            emit reportFailed(QStringLiteral("playback progress"), exceptionMessage(error));
            sendStopIfReady(report);
        },
        "playback progress report");
}

} // namespace Spool
