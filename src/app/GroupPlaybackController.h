#pragma once

#include "../provider/GroupPlayback.h"
#include "GroupClock.h"

#include <QCoroTask>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

namespace JellyfinNative {

class PlayQueueController;
class PlayerController;
class SourceHub;

// Queue handoff: after publishing a queue, unpause only once the group's own
// queue update has arrived and the file is loaded here.
class GroupQueueHandoff final {
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

// A seek pauses the group; resume once everyone reports ready again.
class GroupSeekResume final {
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

struct GroupCorrection {
    enum class Method { None, Speed, Skip };

    Method method = Method::None;
    double speed = 1.0;
    int durationMs = 0;
};

// Mirrors jellyfin-web's PlaybackCore strategy selection so mixed clients
// tolerate and recover the same drift.
class GroupDriftPolicy final {
public:
    // Positive diffMs: this client is behind the group.
    static GroupCorrection evaluate(double diffMs);
};

// Watching together, independent of whose server hosts the group.
//
// The native side keeps everything timing-critical: the clock offset from
// round-trip samples, commands scheduled against server time, drift
// correction by rate or seek, and the ready/buffering handshake. The provider
// keeps the protocol: it owns the socket and turns server messages into a
// small set of `group` events (joined, state, participants, queue, command,
// left, error) and turns the actions below into its server's requests.
class GroupPlaybackController final : public GroupPlayback {
    Q_OBJECT
    Q_PROPERTY(QVariantList groups READ groups NOTIFY groupsChanged)
    Q_PROPERTY(bool loadingGroups READ loadingGroups NOTIFY groupsChanged)
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(QString currentGroupId READ currentGroupId NOTIFY groupChanged)
    Q_PROPERTY(QString currentGroupName READ currentGroupName NOTIFY groupChanged)
    Q_PROPERTY(bool enabled READ enabled NOTIFY groupChanged)
    Q_PROPERTY(QStringList participants READ participants NOTIFY groupChanged)
    Q_PROPERTY(int participantCount READ participantCount NOTIFY groupChanged)
    Q_PROPERTY(QString groupState READ groupState NOTIFY groupChanged)
    Q_PROPERTY(QString groupStateReason READ groupStateReason NOTIFY groupChanged)
    Q_PROPERTY(double clockOffsetMs READ clockOffsetMs NOTIFY clockChanged)
    Q_PROPERTY(double pingMs READ pingMs NOTIFY clockChanged)
    Q_PROPERTY(bool waitingForPlayback READ waitingForPlayback NOTIFY syncStatusChanged)
    Q_PROPERTY(double playbackDiffMs READ playbackDiffMs NOTIFY syncStatusChanged)
    Q_PROPERTY(bool playbackDiffValid READ playbackDiffValid NOTIFY syncStatusChanged)
    Q_PROPERTY(QString syncMethod READ syncMethod NOTIFY syncStatusChanged)

public:
    GroupPlaybackController(
        SourceHub *hub, PlayerController *player, PlayQueueController *playQueue, QObject *parent = nullptr);

    QVariantList groups() const
    {
        return m_groups;
    }
    bool loadingGroups() const
    {
        return m_loadingGroups > 0;
    }
    bool available() const;
    QString currentGroupId() const
    {
        return m_groupId;
    }
    QString currentGroupName() const
    {
        return m_groupName;
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
        return int(m_participants.size());
    }
    QString groupState() const
    {
        return m_groupState;
    }
    QString groupStateReason() const
    {
        return m_groupStateReason;
    }
    double clockOffsetMs() const
    {
        return m_clock.offsetMs();
    }
    double pingMs() const
    {
        return m_clock.pingMs();
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

    // Lists the groups of every account that can host one.
    Q_INVOKABLE void refreshGroups();
    Q_INVOKABLE void createGroup(const QString& accountId, const QString& name);
    // `groupId` is scoped to its account, as refreshGroups() lists it.
    Q_INVOKABLE void joinGroup(const QString& groupId) override;
    Q_INVOKABLE void leaveGroup() override;
    Q_INVOKABLE void requestTogglePause() override;
    Q_INVOKABLE void requestSeek(double positionSeconds) override;
    Q_INVOKABLE void requestRelativeSeek(double deltaSeconds) override;
    Q_INVOKABLE void requestNextItem() override;
    Q_INVOKABLE void requestPreviousItem() override;
    void requestMoveItem(const QString& entryId, int newIndex) override;
    void requestRemoveItems(const QStringList& entryIds) override;
    void requestQueueItems(const QStringList& itemIds, bool queueNext) override;
    void requestPlayItem(const QString& entryId) override;
    void requestUnpauseWhenReady() override;
    void cancelPendingUnpause() override;
    QCoro::Task<void> publishQueue(QStringList itemIds, int playingIndex, qint64 startPositionTicks) override;

    // A `group` event from the account hosting the group.
    void handleEvent(const QString& accountId, const QVariantMap& event);

signals:
    void groupsChanged();
    void availableChanged();
    void groupChanged();
    void clockChanged();
    void syncStatusChanged();
    void errorText(const QString& text);

private:
    void send(const QString& action, QVariantMap arguments = {}, const char *failure = nullptr);
    void handleCommand(const QVariantMap& event);
    void applyQueue(const QVariantMap& event);
    void applyInfo(const QVariantMap& info);
    void clearGroup();
    void executeScheduledCommand();
    void correctPlaybackDrift();
    void finishSpeedCorrection();
    void handlePlayerStateChanged();
    void sendPlayerBufferingState(bool force = false);
    void sendPendingUnpause();
    void requestGroupUnpause();
    void setWaitingForGroupPlayback(bool waiting);
    void setPlaybackDiff(qint64 diffTicks, bool valid);
    void setSyncMethod(const QString& method);
    void beginTimeSync();
    void requestTimeSync();
    qint64 serverNowMs() const;

    SourceHub *m_hub;
    PlayerController *m_player;
    PlayQueueController *m_playQueue;
    QString m_account;
    QString m_groupId;
    QString m_groupName;
    QStringList m_participants;
    QString m_groupState;
    QString m_groupStateReason;
    QVariantList m_groups;
    int m_loadingGroups = 0;
    GroupClock m_clock;
    QTimer m_timeSyncTimer;
    QTimer m_commandTimer;
    QTimer m_correctionTimer;
    QTimer m_speedCorrectionTimer;
    QTimer m_bufferingDebounceTimer;
    QString m_entryId;
    QString m_scheduledEntryId;
    QString m_lastCommandKey;
    QString m_scheduledCommand;
    qint64 m_scheduledPositionTicks = 0;
    qint64 m_scheduledServerTimeMs = 0;
    qint64 m_lastCorrectionAtMs = 0;
    qint64 m_suppressSeekBufferingUntilMs = 0;
    qint64 m_joinedAtServerMs = 0;
    qint64 m_lastQueueUpdateMs = 0;
    double m_playbackDiffMs = 0.0;
    quint64 m_queueGeneration = 0;
    int m_greedyTimeSyncRemaining = 0;
    GroupQueueHandoff m_queueHandoff;
    GroupSeekResume m_seekResume;
    int m_syncCorrectionAttempts = 0;
    bool m_timeSyncInFlight = false;
    bool m_playerStateKnown = false;
    bool m_lastPlayerBuffering = false;
    bool m_queueLoading = false;
    bool m_waitingForPlaybackStart = false;
    bool m_commandDue = false;
    bool m_waitingForGroupPlayback = false;
    bool m_playbackDiffValid = false;
    bool m_unpauseRequestPending = false;
    bool m_speedCorrectionActive = false;
    QString m_syncMethod = QStringLiteral("None");
};

} // namespace JellyfinNative
