#pragma once

#include <QChronoTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>

class QEvent;
class QQuickWindow;
class QScreen;

namespace Spool {

namespace Detail {

    enum class InputLatencyEventKind : quint8 {
        KeyPress,
        KeyRelease,
        MousePress,
        MouseRelease,
        MouseDoubleClick,
        MouseMove,
        Wheel,
        TouchBegin,
        TouchUpdate,
        TouchEnd,
        TouchCancel,
        TabletPress,
        TabletMove,
        TabletRelease,
    };

    enum class InputLatencyStage : quint8 {
        None,
        WaitingForSync,
        Synchronizing,
        Rendering,
        Submitted,
        PresentQueued,
    };

    enum class InputLatencyRefreshSource : quint8 {
        Fallback60,
        Screen,
    };
    enum class InputLatencyRecordSlot : quint8 {
        Pending,
        Current,
    };

    struct InputLatencyEventMetadata {
        InputLatencyEventKind kind = InputLatencyEventKind::KeyPress;
        int key = 0;
        quint32 nativeScanCode = 0;
    };

    struct InputLatencySample {
        quint64 sequence = 0;
        quint64 epoch = 0;
        InputLatencyRecordSlot recordSlot = InputLatencyRecordSlot::Pending;
        InputLatencyEventMetadata event;
        quint64 coalesced = 0;
        qint64 budgetNs = 0;
        InputLatencyRefreshSource refreshSource = InputLatencyRefreshSource::Fallback60;
        qint64 totalNs = 0;
        qint64 dispatchNs = -1;
        qint64 syncBeginNs = -1;
        qint64 syncEndNs = -1;
        qint64 renderEndNs = -1;
        qint64 frameEndNs = -1;
        qint64 presentQueueNs = -1;
        InputLatencyStage stage = InputLatencyStage::None;
        bool late = false;
        bool exposed = false;
    };

    struct InputLatencyExpiredSamples {
        std::array<InputLatencySample, 2> samples;
        std::size_t count = 0;
    };
    struct UiLatencySample {
        QString name;
        QString routeFrom;
        QString routeTo;
        QString cacheHit;
        qint64 budgetNs = 0;
        qint64 totalNs = 0;
        qint64 guiCpuNs = 0;
        qint64 inputNs = -1;
        qint64 presentNs = 0;
        qint64 syncNs = 0;
        qint64 renderNs = 0;
        qint64 swapWaitNs = 0;
        qint64 maxGapNs = 0;
        quint64 budgetIntervals = 0;
        quint64 actualSwaps = 0;
        quint64 delegatesCreated = 0;
        quint64 delegatesDestroyed = 0;
        std::array<qint64, 6> stageNs {};
    };

    std::optional<InputLatencyEventMetadata> classifyInputEvent(const QEvent *event, bool spontaneous);
    QString inputLatencyEventName(InputLatencyEventKind kind);
    QString inputLatencyStageName(InputLatencyStage stage);
    QString formatInputLatencyMiss(const InputLatencySample& sample);
    bool shouldWarnInputLatency(const InputLatencySample& sample);
    QString formatUiLatency(const UiLatencySample& sample);

    class InputLatencyTimeline final {
    public:
        using Nanoseconds = std::chrono::nanoseconds;

        bool enabled() const;
        void setEnabled(bool enabled);

        quint64 beginInput(const InputLatencyEventMetadata& event, Nanoseconds now, Nanoseconds budget,
            InputLatencyRefreshSource refreshSource, bool exposed);
        void endInput(quint64 token, Nanoseconds now);

        void beforeSynchronizing(Nanoseconds now);
        void afterSynchronizing(Nanoseconds now);
        void afterRendering(Nanoseconds now);
        void frameSwapped(Nanoseconds now);
        std::optional<InputLatencySample> afterFrameEnd(Nanoseconds now, bool exposed);

        InputLatencyExpiredSamples expire(Nanoseconds now, bool exposed);
        std::optional<Nanoseconds> nextDeadline() const;

        void cancel();
        void clearStatistics();
        bool publish(const InputLatencySample& sample);

        quint64 sampleCount() const;
        quint64 lateCount() const;
        double lastLatencyMs() const;
        quint64 missedFrameCount() const;
        double worstLatencyMs() const;
        QString lastStage() const;
        quint64 epoch() const;

    private:
        struct PendingRecord {
            bool active = false;
            bool finalized = false;
            quint64 sequence = 0;
            quint64 epoch = 0;
            quint64 firstToken = 0;
            InputLatencyEventMetadata event;
            quint64 coalesced = 0;
            qint64 inputNs = 0;
            qint64 deadlineNs = 0;
            qint64 budgetNs = 0;
            InputLatencyRefreshSource refreshSource = InputLatencyRefreshSource::Fallback60;
            qint64 dispatchNs = -1;
            bool exposed = false;
        };

        struct RenderRecord {
            std::atomic<InputLatencyStage> stage { InputLatencyStage::None };
            std::atomic_bool finalized { false };
            quint64 sequence = 0;
            quint64 epoch = 0;
            InputLatencyEventMetadata event;
            quint64 coalesced = 0;
            qint64 inputNs = 0;
            qint64 deadlineNs = 0;
            qint64 budgetNs = 0;
            InputLatencyRefreshSource refreshSource = InputLatencyRefreshSource::Fallback60;
            qint64 dispatchNs = -1;
            qint64 syncBeginNs = -1;
            qint64 syncEndNs = -1;
            qint64 renderEndNs = -1;
            qint64 frameEndNs = -1;
            qint64 presentQueueNs = -1;
            bool exposed = false;
        };

        InputLatencySample sampleFromPending(qint64 terminalNs) const;
        InputLatencySample sampleFromCurrent(
            qint64 terminalNs, InputLatencyStage publishedStage, bool completedFrame) const;
        void resetCurrentFromPending(qint64 syncBeginNs);
        bool currentMatchesEpoch(InputLatencyStage stage) const;

        std::atomic<quint64> m_epoch { 1 };
        bool m_enabled = false;
        quint64 m_nextToken = 0;
        quint64 m_nextSequence = 0;
        PendingRecord m_pending;
        RenderRecord m_current;
        std::array<std::atomic<quint64>, 2> m_publicationPermits {};

        quint64 m_sampleCount = 0;
        quint64 m_lateCount = 0;
        quint64 m_missedFrameCount = 0;
        qint64 m_lastLatencyNs = 0;
        qint64 m_worstLatencyNs = 0;
        InputLatencyStage m_lastStage = InputLatencyStage::None;
    };

} // namespace Detail

class InputLatencyMonitor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(bool overlayEnabled READ overlayEnabled WRITE setOverlayEnabled NOTIFY overlayEnabledChanged)
    Q_PROPERTY(bool warningVisible READ warningVisible NOTIFY warningChanged)
    Q_PROPERTY(QString warningText READ warningText NOTIFY warningChanged)
    Q_PROPERTY(QString warningStage READ warningStage NOTIFY warningChanged)
    Q_PROPERTY(double lastLatencyMs READ lastLatencyMs NOTIFY statisticsChanged)
    Q_PROPERTY(double worstLatencyMs READ worstLatencyMs NOTIFY statisticsChanged)
    Q_PROPERTY(QString lastStage READ lastStage NOTIFY statisticsChanged)
    Q_PROPERTY(quint64 sampleCount READ sampleCount NOTIFY statisticsChanged)
    Q_PROPERTY(quint64 lateCount READ lateCount NOTIFY statisticsChanged)
    Q_PROPERTY(quint64 missedFrameCount READ missedFrameCount NOTIFY statisticsChanged)
    Q_PROPERTY(double frameBudgetMs READ frameBudgetMs NOTIFY frameBudgetChanged)
    Q_PROPERTY(QString lastRouteSample READ lastRouteSample NOTIFY routeSampleChanged)
    // The same numbers as the log line, in a shape a harness can read
    // without parsing prose that was written for a person.
    Q_PROPERTY(QVariantMap lastRouteMetrics READ lastRouteMetrics NOTIFY routeSampleChanged)

public:
    explicit InputLatencyMonitor(QObject *parent = nullptr);

    void attachWindow(QQuickWindow *window);
    quint64 beginInput(const QEvent *event);
    void endInput(quint64 token);

    bool enabled() const;
    void setEnabled(bool enabled);
    bool overlayEnabled() const;
    void setOverlayEnabled(bool enabled);
    bool warningVisible() const;
    QString warningText() const;
    QString warningStage() const;
    double lastLatencyMs() const;
    double worstLatencyMs() const;
    QString lastStage() const;
    quint64 sampleCount() const;
    quint64 lateCount() const;
    quint64 missedFrameCount() const;
    double frameBudgetMs() const;
    QString lastRouteSample() const;
    QVariantMap lastRouteMetrics() const;

    Q_INVOKABLE void clearStatistics();
    Q_INVOKABLE quint64 beginUiTransition(
        const QString& name, const QString& routeFrom = {}, const QString& routeTo = {}, const QString& cacheHit = {});
    Q_INVOKABLE void mark(quint64 token, const QString& stage);
    Q_INVOKABLE void noteDelegate(const QString& kind, int delta);

signals:
    void enabledChanged();
    void overlayEnabledChanged();
    void warningChanged();
    void statisticsChanged();
    void frameBudgetChanged();
    void routeSampleChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    static Detail::InputLatencyTimeline::Nanoseconds now();
    void updateScreen(QScreen *screen);
    void updateFrameBudget();
    void scheduleDeadline();
    void handleDeadline();
    void handleCompletedSample(const Detail::InputLatencySample& sample);
    void handleUiTransitionFrame(qint64 frameNs);
    void handleUiGapTimer();
    void resetUiTransition();
    void cancelMeasurements();
    void finishCancellationOnGuiThread();
    void hideWarning();
    bool canCaptureInput() const;

    Detail::InputLatencyTimeline m_timeline;
    QQuickWindow *m_window = nullptr;
    QScreen *m_screen = nullptr;
    QMetaObject::Connection m_refreshRateConnection;
    QChronoTimer m_deadlineTimer;
    QTimer m_warningTimer;
    qint64 m_frameBudgetNs = 16670000;
    Detail::InputLatencyRefreshSource m_refreshSource = Detail::InputLatencyRefreshSource::Fallback60;
    QString m_warningText;
    QString m_warningStage;
    qint64 m_warningLatencyNs = 0;
    bool m_warningVisible = false;
    bool m_overlayEnabled = true;
    struct UiTransition {
        enum class Stage : quint8 { Instance, Shell, ModelReady, FirstDelegate, Viewport, ContentReady, Count };
        struct Mark {
            qint64 elapsedNs = -1;
            qint64 cpuNs = -1;
        };

        quint64 sequence = 0;
        quint64 token = 0;
        QString name;
        QString routeFrom;
        QString routeTo;
        QString cacheHit;
        qint64 beginNs = -1;
        qint64 beginCpuNs = -1;
        qint64 inputBeginNs = -1;
        qint64 expectedGapFireNs = -1;
        qint64 maxGapNs = 0;
        quint64 delegatesCreated = 0;
        quint64 delegatesDestroyed = 0;
        QHash<QString, qint64> delegateCounts;
        std::array<Mark, static_cast<std::size_t>(Stage::Count)> marks;

        void reset();
    };

    UiTransition m_uiTransition;
    QVariantMap m_lastRouteMetrics;
    QChronoTimer m_uiGapTimer;
    QString m_lastRouteSample;
    qint64 m_lastInputBeginNs = -1;
    std::atomic<quint32> m_uiActualSwaps { 0 };
    std::atomic<qint64> m_uiSyncBeginNs { -1 };
    std::atomic<qint64> m_uiRenderBeginNs { -1 };
    std::atomic<qint64> m_uiSyncNs { 0 };
    std::atomic<qint64> m_uiRenderNs { 0 };
    std::atomic<qint64> m_uiSwapWaitNs { 0 };
    // Read on the render thread each frame swap; queue work to the GUI
    // thread only while a transition is actually being measured.
    std::atomic_bool m_uiTransitionActive { false };
    std::atomic_bool m_hasPresentedFrame { false };
};

} // namespace Spool
