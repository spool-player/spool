#pragma once

#include "../media/MediaTypes.h"
#include "../provider/PlaybackSource.h"

#include <QHash>
#include <QObject>
#include <QTimer>
#include <QtTypes>

#include <memory>

namespace Spool {

class PlaybackReporter final : public QObject {
    Q_OBJECT

public:
    explicit PlaybackReporter(PlaybackSource *api, QObject *parent = nullptr);

    void start(const PlaybackSession& session, double playbackRate, int volume, bool muted);
    bool setStreamIndexes(int audioStreamIndex, int subtitleStreamIndex);
    void reportProgress(qint64 positionTicks, bool paused, double playbackRate, int volume, bool muted);
    quint64 stop(qint64 positionTicks, bool failed, double playbackRate = 1.0, bool watched = false);

signals:
    void reportFailed(const QString& operation, const QString& message);
    // Emitted after this session and older same-item sessions drain, including
    // final stop attempts. A late report cannot then undo the newest played
    // mutation or restore its cleared resume position.
    void watchedPersistenceRequested(const QString& itemId, quint64 reportId);

private:
    struct Report {
        PlaybackSession session;
        quint64 id = 0;
        qint64 stopPositionTicks = 0;
        double stopPlaybackRate = 1.0;
        bool active = true;
        bool startInFlight = false;
        bool startReported = false;
        bool progressInFlight = false;
        bool stopPending = false;
        bool stopFailed = false;
        bool watched = false;
        bool stopFinished = false;
        std::shared_ptr<Report> predecessor;
        std::weak_ptr<Report> successor;
    };
    using ReportPtr = std::shared_ptr<Report>;

    void sendStart();
    void sendProgress();
    void sendStopIfReady(const ReportPtr& report);
    void sendStop(const ReportPtr& report, int attempt);
    void finishStop(const ReportPtr& report);

    PlaybackSource *m_api = nullptr;
    ReportPtr m_report;
    quint64 m_nextReportId = 0;
    // Only outstanding stop operations, not item/progress state. Same-item
    // stops finish in session order before the newest played mutation.
    QHash<QString, ReportPtr> m_stopTails;
    QTimer m_startRetryTimer;
    QTimer m_progressRetryTimer;
    qint64 m_pendingPositionTicks = 0;
    bool m_pendingPaused = false;
    double m_pendingPlaybackRate = 1.0;
    double m_startPlaybackRate = 1.0;
    int m_startVolume = 100;
    int m_pendingVolume = 100;
    bool m_startMuted = false;
    bool m_pendingMuted = false;
    bool m_progressPending = false;
};

} // namespace Spool
