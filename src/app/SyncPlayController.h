#pragma once

#include "../provider/GroupPlayback.h"
#include "SyncPlayClock.h"

#include <QCoroTask>

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWebSocket>

namespace JellyfinNative {

class JellyfinApiFacade;
class PlayerController;
class PlayQueueController;
class TlsTrustController;

class SyncPlayQueueHandoff final {
public:
    void arm()
    {
        m_armed = true;
        m_queueUpdateObserved = false;
    }
    void observeQueueUpdate()
    {
        if (m_armed)
            m_queueUpdateObserved = true;
    }
    void cancel()
    {
        m_armed = false;
        m_queueUpdateObserved = false;
    }
    bool canSend(bool queueLoading, bool playbackStarting, bool sessionActive, bool fileLoaded) const
    {
        return m_armed && m_queueUpdateObserved && !queueLoading && !playbackStarting && sessionActive && fileLoaded;
    }

private:
    bool m_armed = false;
    bool m_queueUpdateObserved = false;
};

class SyncPlaySeekResume final {
public:
    void arm(bool groupWasPlaying)
    {
        m_pending = m_pending || groupWasPlaying;
    }
    bool takeWhenReady(const QString& state, const QString& reason)
    {
        if (!m_pending || state != QStringLiteral("Paused") || reason != QStringLiteral("Ready"))
            return false;
        m_pending = false;
        return true;
    }
    void cancel()
    {
        m_pending = false;
    }

private:
    bool m_pending = false;
};

struct SyncCorrection {
    enum class Method { None, Speed, Skip };

    Method method = Method::None;
    double speed = 1.0;
    int durationMs = 0;
};

// Mirrors the strategy selection in jellyfin-web
// src/plugins/syncPlay/core/PlaybackCore.js so both clients tolerate and
// recover the same amount of drift.
class SyncPlayDriftPolicy final {
public:
    // diffMs is the estimated server position minus the local position, so a
    // positive value means this client is behind the group.
    static SyncCorrection evaluate(double diffMs);
};

class SyncPlayController final : public GroupPlayback {
    Q_OBJECT
    Q_PROPERTY(QString currentGroupId READ currentGroupId NOTIFY groupChanged)
    Q_PROPERTY(QString currentGroupName READ currentGroupName NOTIFY groupChanged)
    Q_PROPERTY(QJsonArray groups READ groups NOTIFY groupsChanged)
    Q_PROPERTY(bool enabled READ enabled NOTIFY groupChanged)
    Q_PROPERTY(QStringList participants READ participants NOTIFY groupChanged)
    Q_PROPERTY(int participantCount READ participantCount NOTIFY groupChanged)
    Q_PROPERTY(QString groupState READ groupState NOTIFY groupChanged)
    Q_PROPERTY(QString groupStateReason READ groupStateReason NOTIFY groupChanged)
    Q_PROPERTY(bool socketConnected READ socketConnected NOTIFY connectionChanged)
    Q_PROPERTY(double clockOffsetMs READ clockOffsetMs NOTIFY connectionChanged)
    Q_PROPERTY(double pingMs READ pingMs NOTIFY connectionChanged)
    Q_PROPERTY(QString timeSyncDevice READ timeSyncDevice CONSTANT)
    Q_PROPERTY(bool waitingForPlayback READ waitingForPlayback NOTIFY syncStatusChanged)
    Q_PROPERTY(double playbackDiffMs READ playbackDiffMs NOTIFY syncStatusChanged)
    Q_PROPERTY(bool playbackDiffValid READ playbackDiffValid NOTIFY syncStatusChanged)
    Q_PROPERTY(QString syncMethod READ syncMethod NOTIFY syncStatusChanged)

public:
    SyncPlayController(JellyfinApiFacade *api, PlayerController *player, PlayQueueController *playQueue,
        TlsTrustController *tlsTrust, QObject *parent = nullptr);

    QString currentGroupId() const
    {
        return m_groupId;
    }
    QString currentGroupName() const
    {
        return m_groupName;
    }
    QJsonArray groups() const
    {
        return m_groups;
    }
    bool enabled() const override
    {
        return !m_groupId.isEmpty();
    }
    QStringList participants() const
    {
        return m_participants;
    }
    int participantCount() const
    {
        return m_participants.size();
    }
    QString groupState() const
    {
        return m_groupState;
    }
    QString groupStateReason() const
    {
        return m_groupStateReason;
    }
    bool socketConnected() const;
    double clockOffsetMs() const
    {
        return m_clock.offsetMs();
    }
    double pingMs() const
    {
        return m_clock.pingMs();
    }
    QString timeSyncDevice() const
    {
        return QStringLiteral("Jellyfin server");
    }
    bool waitingForPlayback() const
    {
        return m_waitingForGroupPlayback;
    }
    double playbackDiffMs() const
    {
        return m_playbackDiffMs;
    }
    bool playbackDiffValid() const
    {
        return m_playbackDiffValid;
    }
    QString syncMethod() const
    {
        return m_syncMethod;
    }

    Q_INVOKABLE void refreshGroups();
    Q_INVOKABLE void connectSocket();
    Q_INVOKABLE void disconnectSocket();
    Q_INVOKABLE void createGroup(const QString& name);
    Q_INVOKABLE void joinGroup(const QString& groupId) override;
    Q_INVOKABLE void leaveGroup() override;
    Q_INVOKABLE void requestTogglePause() override;
    Q_INVOKABLE void requestSeek(double positionSeconds) override;
    Q_INVOKABLE void requestRelativeSeek(double deltaSeconds) override;
    Q_INVOKABLE void requestNextItem() override;
    Q_INVOKABLE void requestPreviousItem() override;
    // Inside a group the queue belongs to the server. These publish an intent
    // and the resulting PlayQueue broadcast is what actually changes the queue.
    void requestMoveItem(const QString& playlistItemId, int newIndex) override;
    void requestRemoveItems(const QStringList& playlistItemIds) override;
    void requestQueueItems(const QStringList& itemIds, bool queueNext) override;
    void requestPlayItem(const QString& playlistItemId) override;
    void requestUnpauseWhenReady() override;
    void cancelPendingUnpause() override;
    QCoro::Task<void> publishQueue(QStringList itemIds, int playingIndex, qint64 startPositionTicks) override;

signals:
    void groupsChanged();
    void groupChanged();
    void connectionChanged();
    void syncStatusChanged();
    void errorText(const QString& text);
    void remotePlayCommand(const QJsonObject& data);
    void remotePlaystateCommand(const QJsonObject& data);
    void remoteGeneralCommand(const QJsonObject& data);
    void sessionsUpdated(const QJsonArray& sessions);

private:
    void handleSocketTextMessage(const QString& message);
    void handleSyncPlayCommand(const QJsonObject& data);
    void handleSyncPlayGroupUpdate(const QJsonObject& data);
    void applyPlayQueueUpdate(const QJsonObject& queue);
    void prepareQueuePlayback(qint64 positionTicks);
    void sendPendingUnpause();
    void requestGroupUnpause();
    void setWaitingForGroupPlayback(bool waiting);
    void setPlaybackDiff(qint64 diffTicks, bool valid);
    void setSyncMethod(const QString& method);
    void applyGroupInfo(const QString& groupId, const QJsonObject& info);
    void clearGroup();
    void executeScheduledCommand();
    void correctPlaybackDrift();
    void finishSpeedCorrection();
    void handlePlayerStateChanged();
    void sendPlayerBufferingState(bool force = false);
    void beginTimeSync();
    void requestTimeSync();
    void scheduleTimeSync();
    void reportRequestError(const QString& action, const std::exception_ptr& error);
    void sendKeepAlive();
    QUrl socketUrl() const;

    JellyfinApiFacade *m_api = nullptr;
    PlayerController *m_player = nullptr;
    PlayQueueController *m_playQueue = nullptr;
    QWebSocket m_socket;
    QString m_groupId;
    QString m_groupName;
    QStringList m_participants;
    QString m_groupState;
    QString m_groupStateReason;
    QJsonArray m_groups;
    SyncPlayClock m_clock;
    QTimer m_timeSyncTimer;
    QTimer m_commandTimer;
    QTimer m_correctionTimer;
    QTimer m_speedCorrectionTimer;
    QTimer m_reconnectTimer;
    QTimer m_bufferingDebounceTimer;
    QString m_playlistItemId;
    QString m_scheduledPlaylistItemId;
    QString m_lastCommandKey;
    QString m_scheduledCommand;
    qint64 m_scheduledPositionTicks = 0;
    qint64 m_scheduledServerTimeMs = 0;
    qint64 m_lastCorrectionAtMs = 0;
    qint64 m_suppressSeekBufferingUntilMs = 0;
    qint64 m_joinedAtServerMs = 0;
    qint64 m_lastPlayQueueUpdateMs = 0;
    double m_playbackDiffMs = 0.0;
    quint64 m_playQueueGeneration = 0;
    int m_greedyTimeSyncRemaining = 0;
    SyncPlayQueueHandoff m_queueHandoff;
    SyncPlaySeekResume m_seekResume;
    int m_syncCorrectionAttempts = 0;
    bool m_socketDesired = false;
    // Consecutive socket failures with a reconnect still wanted; reset by a
    // successful connect and by giving up on the socket deliberately.
    int m_socketFailures = 0;
    bool m_timeSyncInFlight = false;
    bool m_timeSyncErrorReported = false;
    bool m_playerStateKnown = false;
    bool m_lastPlayerBuffering = false;
    bool m_playQueueLoading = false;
    bool m_waitingForPlaybackStart = false;
    bool m_commandDue = false;
    bool m_waitingForGroupPlayback = false;
    bool m_playbackDiffValid = false;
    bool m_unpauseRequestPending = false;
    bool m_speedCorrectionActive = false;
    QString m_syncMethod = QStringLiteral("None");
};

} // namespace JellyfinNative
