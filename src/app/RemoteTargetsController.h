#pragma once

#include <QCoroTask>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <memory>
#include <optional>

namespace Spool {
class SourceHub;
class ProviderRegistry;
class GroupPlaybackController;
class ProviderUiContext;
class PlatformRemoteMediaSession;
class TrickplayService;

// Outbound control only. Neither selection nor a remote snapshot is an inbound
// playback command; every asynchronous result belongs to one selection epoch.
class RemoteTargetsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList targets READ targets NOTIFY targetsChanged)
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(QString selectedTargetId READ selectedTargetId NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selectedTarget READ selectedTarget NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString positionTicks READ positionTicks NOTIFY positionChanged)
    Q_PROPERTY(QString runtimeTicks READ runtimeTicks NOTIFY positionChanged)
    Q_PROPERTY(QVariantList queue READ queue NOTIFY queueChanged)
    Q_PROPERTY(bool queueHasMore READ queueHasMore NOTIFY queueChanged)
    Q_PROPERTY(bool queueBusy READ queueBusy NOTIFY queueChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString problem READ problem NOTIFY problemChanged)
    Q_PROPERTY(bool trickplayAvailable READ trickplayAvailable NOTIFY trickplayChanged)
    Q_PROPERTY(bool chooserVisible READ chooserVisible NOTIFY visibilityChanged)
    Q_PROPERTY(bool queueVisible READ queueVisible NOTIFY visibilityChanged)
    Q_PROPERTY(
        bool groupLeaveConfirmationPending READ groupLeaveConfirmationPending NOTIFY groupLeaveConfirmationChanged)
public:
    struct Selection {
        QString targetId;
        QString accountId;
        quint64 generation;
    };
    RemoteTargetsController(SourceHub *, ProviderRegistry *, GroupPlaybackController *, QObject *parent = nullptr);
    ~RemoteTargetsController() override;
    QVariantList targets() const
    {
        return m_targets;
    }
    bool available() const;
    QString selectedTargetId() const
    {
        return m_selection.targetId;
    }
    QVariantMap selectedTarget() const
    {
        return m_target;
    }
    QVariantMap state() const;
    // The running clock for media-session consumers: the last accepted
    // position advanced by elapsed time while the target plays. Zero is a
    // real position, not a frozen clock.
    qint64 predictedPositionTicks() const;
    QString positionTicks() const
    {
        return m_position;
    }
    QString runtimeTicks() const
    {
        return m_runtime;
    }
    QVariantList queue() const
    {
        return m_queue;
    }
    bool queueHasMore() const
    {
        return !m_queueExhausted;
    }
    bool queueBusy() const
    {
        return m_queueBusy;
    }
    bool busy() const
    {
        return m_connecting || m_commandBusy || m_listActive > 0;
    }
    QString problem() const
    {
        return m_problem;
    }
    bool chooserVisible() const
    {
        return m_chooserVisible;
    }
    bool queueVisible() const
    {
        return m_queueVisible;
    }
    bool groupLeaveConfirmationPending() const
    {
        return !m_pendingTarget.isEmpty();
    }
    Selection selection() const
    {
        return m_selection;
    }
    bool isCurrent(const Selection&) const;
    QCoro::Task<bool> play(
        Selection, QStringList scopedItemIds, int index, QString positionTicks, QString mode, QString variantId = {});
    Q_INVOKABLE void refreshTargets();
    Q_INVOKABLE void selectTarget(QString targetId);
    Q_INVOKABLE void disconnectTarget();
    Q_INVOKABLE void send(QVariantMap command);
    Q_INVOKABLE void requestQueuePage(bool reset);
    Q_INVOKABLE void setControlsVisible(bool visible);
    Q_INVOKABLE void setChooserVisible(bool visible);
    Q_INVOKABLE void setQueueVisible(bool visible);
    Q_INVOKABLE void confirmLeaveGroup(bool accepted);
    Q_INVOKABLE void openAdvancedControls();
    Q_INVOKABLE QObject *createAdvancedControls();
    // Also used by lifecycle adapters on platforms without QGuiApplication.
    void setForeground(bool foreground);
    void setTrickplayService(TrickplayService *service);
    Q_INVOKABLE QVariantMap trickplayForSeconds(double seconds) const;
    bool trickplayAvailable() const;
signals:
    void targetsChanged();
    void availableChanged();
    void selectionChanged();
    void stateChanged();
    void positionChanged();
    void queueChanged();
    void busyChanged();
    void problemChanged();
    void visibilityChanged();
    void groupLeaveConfirmationChanged();
    void trickplayChanged();

private:
    void updateTimers();
    void launchListRequests();
    void finishList(QString account, quint64 generation, QVariantList targets, bool failed);
    void rebuildTargets();
    void connectTarget(QVariantMap target);
    QCoro::Task<void> connectSelected(Selection selection, QVariantMap target);
    void requestState();
    void applyState(QVariantMap state);
    void failTarget(const Selection&, const QString& problem);
    void setProblem(QString problem);
    void clearQueue();
    void updateMediaSession();
    void beginOptimistic(const QVariantMap& command);
    QCoro::Task<bool> command(Selection selection, QVariantMap command);
    QCoro::Task<void> loadQueue(Selection selection, bool reset, quint64 queueGeneration);
    void cancelSelection();

    SourceHub *m_hub;
    ProviderRegistry *m_registry;
    GroupPlaybackController *m_group;
    QPointer<ProviderUiContext> m_advancedControls;
    std::unique_ptr<PlatformRemoteMediaSession> m_mediaSession;
    Selection m_selection { {}, {}, 0 };
    QVariantMap m_target;
    QVariantMap m_state;
    QString m_position;
    QString m_runtime;
    QVariantList m_targets;
    TrickplayService *m_trickplay = nullptr;
    QHash<QString, QVariantList> m_accountTargets;
    QStringList m_listAccounts;
    QSet<QString> m_listPending;
    int m_listNext = 0;
    int m_listActive = 0;
    quint64 m_listGeneration = 0;
    quint64 m_commandEpoch = 0;
    quint64 m_queueGeneration = 0;
    quint64 m_readSerial = 0;
    bool m_connecting = false;
    bool m_commandBusy = false;
    bool m_readBusy = false;
    bool m_readAgain = false;
    bool m_controlsVisible = false;
    bool m_chooserVisible = false;
    bool m_queueVisible = false;
    bool m_foreground = true;
    QString m_problem;
    QString m_pendingTarget;
    bool m_leavingGroup = false;
    QVariantList m_queue;
    bool m_queueBusy = false;
    bool m_queueResetPending = false;
    bool m_queueExhausted = true;
    int m_queuePages = 0;
    std::optional<QString> m_cursor;
    QSet<QString> m_seenCursors;
    QString m_loadedRevision;
    QElapsedTimer m_clock;
    qint64 m_lastQueueRead = -5000;
    QVariantMap m_optimistic;
    qint64 m_optimisticUntil = 0;
    qint64 m_optimisticAt = 0;
    qint64 m_positionAt = 0;
    std::optional<quint64> m_ackSequence;
    QTimer m_pollTimer;
    QTimer m_listTimer;
    QTimer m_queueTimer;
};
} // namespace Spool
