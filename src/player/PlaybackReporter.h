#pragma once

#include "../media/MediaTypes.h"

#include <QObject>
#include <QTimer>
#include <QtTypes>

namespace JellyfinNative {

class JellyfinApiFacade;

class PlaybackReporter final : public QObject {
    Q_OBJECT

public:
    explicit PlaybackReporter(JellyfinApiFacade *api, QObject *parent = nullptr);

    void start(const PlaybackSession& session, double playbackRate, int volume, bool muted);
    bool setStreamIndexes(int audioStreamIndex, int subtitleStreamIndex);
    void reportProgress(qint64 positionTicks, bool paused, double playbackRate, int volume, bool muted);
    void stop(qint64 positionTicks, bool failed, double playbackRate = 1.0);

signals:
    void reportFailed(const QString& operation, const QString& message);

private:
    void sendStart();
    void sendProgress();
    void sendStop(const PlaybackSession& session, qint64 positionTicks, bool failed, double playbackRate, int attempt);

    JellyfinApiFacade *m_api = nullptr;
    PlaybackSession m_session;
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
    bool m_active = false;
    bool m_startInFlight = false;
    bool m_startReported = false;
    bool m_progressInFlight = false;
    bool m_progressPending = false;
    quint64 m_generation = 0;
};

} // namespace JellyfinNative
