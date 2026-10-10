#include "InputLatencyMonitor.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QInputDevice>
#include <QInputEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QScreen>
#include <QSettings>
#include <QTabletEvent>
#include <QTouchEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <ctime>

namespace Spool::Detail {

namespace {

    constexpr qint64 kNanosecondsPerMillisecond = 1000000;

    qint64 toNanoseconds(InputLatencyTimeline::Nanoseconds value)
    {
        return value.count();
    }

    double toMilliseconds(qint64 nanoseconds)
    {
        return static_cast<double>(nanoseconds) / static_cast<double>(kNanosecondsPerMillisecond);
    }

    QString millisecondsText(qint64 nanoseconds)
    {
        return QString::number(toMilliseconds(nanoseconds), 'f', 2);
    }

    void appendElapsed(QStringList& fields, QLatin1StringView name, qint64 elapsedNs)
    {
        if (elapsedNs >= 0)
            fields.append(QStringLiteral("%1=%2").arg(name, millisecondsText(elapsedNs)));
    }

    bool isMouseEvent(QEvent::Type type)
    {
        return type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease
            || type == QEvent::MouseButtonDblClick || type == QEvent::MouseMove;
    }

} // namespace

std::optional<InputLatencyEventMetadata> classifyInputEvent(const QEvent *event, bool spontaneous)
{
    if (!event || !spontaneous)
        return std::nullopt;

    InputLatencyEventMetadata metadata;
    switch (event->type()) {
    case QEvent::KeyPress: {
        const auto *keyEvent = static_cast<const QKeyEvent *>(event);
        metadata.kind = InputLatencyEventKind::KeyPress;
        metadata.key = keyEvent->key();
        metadata.nativeScanCode = keyEvent->nativeScanCode();
        break;
    }
    case QEvent::KeyRelease: {
        const auto *keyEvent = static_cast<const QKeyEvent *>(event);
        metadata.kind = InputLatencyEventKind::KeyRelease;
        metadata.key = keyEvent->key();
        metadata.nativeScanCode = keyEvent->nativeScanCode();
        break;
    }
    case QEvent::MouseButtonPress:
        metadata.kind = InputLatencyEventKind::MousePress;
        break;
    case QEvent::MouseButtonRelease:
        metadata.kind = InputLatencyEventKind::MouseRelease;
        break;
    case QEvent::MouseButtonDblClick:
        metadata.kind = InputLatencyEventKind::MouseDoubleClick;
        break;
    case QEvent::MouseMove:
        metadata.kind = InputLatencyEventKind::MouseMove;
        break;
    case QEvent::Wheel:
        metadata.kind = InputLatencyEventKind::Wheel;
        break;
    case QEvent::TouchBegin:
        metadata.kind = InputLatencyEventKind::TouchBegin;
        break;
    case QEvent::TouchUpdate:
        metadata.kind = InputLatencyEventKind::TouchUpdate;
        break;
    case QEvent::TouchEnd:
        metadata.kind = InputLatencyEventKind::TouchEnd;
        break;
    case QEvent::TouchCancel:
        metadata.kind = InputLatencyEventKind::TouchCancel;
        break;
    case QEvent::TabletPress:
        metadata.kind = InputLatencyEventKind::TabletPress;
        break;
    case QEvent::TabletMove:
        metadata.kind = InputLatencyEventKind::TabletMove;
        break;
    case QEvent::TabletRelease:
        metadata.kind = InputLatencyEventKind::TabletRelease;
        break;
    default:
        return std::nullopt;
    }

    if (isMouseEvent(event->type())) {
        const auto *inputEvent = static_cast<const QInputEvent *>(event);
        if (inputEvent->deviceType() == QInputDevice::DeviceType::TouchScreen)
            return std::nullopt;
    }

    return metadata;
}

QString inputLatencyEventName(InputLatencyEventKind kind)
{
    switch (kind) {
    case InputLatencyEventKind::KeyPress:
        return QStringLiteral("key_press");
    case InputLatencyEventKind::KeyRelease:
        return QStringLiteral("key_release");
    case InputLatencyEventKind::MousePress:
        return QStringLiteral("mouse_press");
    case InputLatencyEventKind::MouseRelease:
        return QStringLiteral("mouse_release");
    case InputLatencyEventKind::MouseDoubleClick:
        return QStringLiteral("mouse_double_click");
    case InputLatencyEventKind::MouseMove:
        return QStringLiteral("mouse_move");
    case InputLatencyEventKind::Wheel:
        return QStringLiteral("wheel");
    case InputLatencyEventKind::TouchBegin:
        return QStringLiteral("touch_begin");
    case InputLatencyEventKind::TouchUpdate:
        return QStringLiteral("touch_update");
    case InputLatencyEventKind::TouchEnd:
        return QStringLiteral("touch_end");
    case InputLatencyEventKind::TouchCancel:
        return QStringLiteral("touch_cancel");
    case InputLatencyEventKind::TabletPress:
        return QStringLiteral("tablet_press");
    case InputLatencyEventKind::TabletMove:
        return QStringLiteral("tablet_move");
    case InputLatencyEventKind::TabletRelease:
        return QStringLiteral("tablet_release");
    }
    return QStringLiteral("unknown");
}

QString inputLatencyStageName(InputLatencyStage stage)
{
    switch (stage) {
    case InputLatencyStage::WaitingForSync:
        return QStringLiteral("waiting_for_sync");
    case InputLatencyStage::Synchronizing:
        return QStringLiteral("synchronizing");
    case InputLatencyStage::Rendering:
        return QStringLiteral("rendering");
    case InputLatencyStage::Submitted:
        return QStringLiteral("submitted");
    case InputLatencyStage::PresentQueued:
        return QStringLiteral("present_queued");
    case InputLatencyStage::None:
        break;
    }
    return QString();
}

QString formatInputLatencyMiss(const InputLatencySample& sample)
{
    QStringList fields;
    fields.reserve(17);
    fields.append(QStringLiteral("input latency: event=%1").arg(inputLatencyEventName(sample.event.kind)));
    fields.append(QStringLiteral("key=%1").arg(sample.event.key));
    fields.append(QStringLiteral("scan=%1").arg(sample.event.nativeScanCode));
    fields.append(QStringLiteral("coalesced=%1").arg(sample.coalesced));
    fields.append(QStringLiteral("forced_update=0"));
    fields.append(QStringLiteral("budget_ms=%1").arg(millisecondsText(sample.budgetNs)));
    fields.append(QStringLiteral("refresh_source=%1")
            .arg(sample.refreshSource == InputLatencyRefreshSource::Screen ? QStringLiteral("screen")
                                                                           : QStringLiteral("fallback60")));
    fields.append(QStringLiteral("total_ms=%1").arg(millisecondsText(sample.totalNs)));
    appendElapsed(fields, QLatin1StringView("dispatch_ms"), sample.dispatchNs);
    appendElapsed(fields, QLatin1StringView("sync_begin_ms"), sample.syncBeginNs);
    appendElapsed(fields, QLatin1StringView("sync_end_ms"), sample.syncEndNs);
    appendElapsed(fields, QLatin1StringView("render_end_ms"), sample.renderEndNs);
    appendElapsed(fields, QLatin1StringView("frame_end_ms"), sample.frameEndNs);
    appendElapsed(fields, QLatin1StringView("present_queue_ms"), sample.presentQueueNs);
    fields.append(QStringLiteral("stage=%1").arg(inputLatencyStageName(sample.stage)));
    fields.append(QStringLiteral("exposed=%1").arg(sample.exposed ? 1 : 0));
    return fields.join(QLatin1Char(' '));
}

bool shouldWarnInputLatency(const InputLatencySample& sample)
{
    if (!sample.late)
        return false;
    qint64 thresholdNs = sample.budgetNs * 2;
    if (sample.event.kind == InputLatencyEventKind::MouseMove)
        thresholdNs = std::max<qint64>(thresholdNs, 100'000'000);
    return sample.totalNs > thresholdNs;
}

QString formatUiLatency(const UiLatencySample& sample)
{
    static constexpr std::array<QLatin1StringView, 6> stageNames { QLatin1StringView("instance_ms"),
        QLatin1StringView("shell_ms"), QLatin1StringView("model_ready_ms"), QLatin1StringView("first_delegate_ms"),
        QLatin1StringView("viewport_ms"), QLatin1StringView("content_ready_ms") };
    QStringList fields;
    fields.reserve(26);
    fields << QStringLiteral("ui latency: name=%1").arg(sample.name)
           << QStringLiteral("route_from=%1").arg(sample.routeFrom) << QStringLiteral("route_to=%1").arg(sample.routeTo)
           << QStringLiteral("cache_hit=%1").arg(sample.cacheHit)
           << QStringLiteral("refresh_budget_ms=%1").arg(millisecondsText(sample.budgetNs))
           << QStringLiteral("wall_ms=%1").arg(millisecondsText(sample.totalNs))
           << QStringLiteral("gui_cpu_ms=%1").arg(millisecondsText(sample.guiCpuNs));
    appendElapsed(fields, QLatin1StringView("input_ms"), sample.inputNs);
    for (std::size_t index = 0; index < sample.stageNs.size(); ++index)
        appendElapsed(fields, stageNames[index], sample.stageNs[index]);
    fields << QStringLiteral("present_ms=%1").arg(millisecondsText(sample.presentNs))
           << QStringLiteral("budget_intervals=%1").arg(sample.budgetIntervals)
           << QStringLiteral("actual_swaps=%1").arg(sample.actualSwaps)
           << QStringLiteral("sync_ms_total=%1").arg(millisecondsText(sample.syncNs))
           << QStringLiteral("render_ms_total=%1").arg(millisecondsText(sample.renderNs))
           << QStringLiteral("swap_wait_ms=%1").arg(millisecondsText(sample.swapWaitNs))
           << QStringLiteral("delegates_created=%1").arg(sample.delegatesCreated)
           << QStringLiteral("delegates_destroyed=%1").arg(sample.delegatesDestroyed)
           << QStringLiteral("max_gap_ms=%1").arg(millisecondsText(sample.maxGapNs))
           << QStringLiteral("stage=content_presented");
    return fields.join(QLatin1Char(' '));
}

bool InputLatencyTimeline::enabled() const
{
    return m_enabled;
}

void InputLatencyTimeline::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    cancel();
}

quint64 InputLatencyTimeline::beginInput(const InputLatencyEventMetadata& event, Nanoseconds now, Nanoseconds budget,
    InputLatencyRefreshSource refreshSource, bool exposed)
{
    if (!m_enabled)
        return 0;

    const quint64 token = ++m_nextToken;
    if (m_pending.active) {
        ++m_pending.coalesced;
        return token;
    }

    m_pending.active = true;
    m_pending.finalized = false;
    m_pending.sequence = ++m_nextSequence;
    m_pending.epoch = m_epoch.load(std::memory_order_acquire);
    m_pending.firstToken = token;
    m_pending.event = event;
    m_pending.coalesced = 0;
    m_pending.inputNs = toNanoseconds(now);
    m_pending.budgetNs = std::max<qint64>(1, toNanoseconds(budget));
    m_pending.deadlineNs = m_pending.inputNs + m_pending.budgetNs;
    m_pending.refreshSource = refreshSource;
    m_pending.dispatchNs = -1;
    m_pending.exposed = exposed;
    return token;
}

void InputLatencyTimeline::endInput(quint64 token, Nanoseconds now)
{
    if (token == 0 || !m_pending.active || m_pending.firstToken != token || m_pending.dispatchNs >= 0)
        return;
    m_pending.dispatchNs = std::max<qint64>(0, toNanoseconds(now) - m_pending.inputNs);
}

void InputLatencyTimeline::resetCurrentFromPending(qint64 syncBeginNs)
{
    m_current.sequence = m_pending.sequence;
    m_current.epoch = m_pending.epoch;
    m_current.event = m_pending.event;
    m_current.coalesced = m_pending.coalesced;
    m_current.inputNs = m_pending.inputNs;
    m_current.deadlineNs = m_pending.deadlineNs;
    m_current.budgetNs = m_pending.budgetNs;
    m_current.refreshSource = m_pending.refreshSource;
    m_current.dispatchNs = m_pending.dispatchNs;
    m_current.exposed = m_pending.exposed;
    m_current.syncBeginNs = syncBeginNs;
    m_current.syncEndNs = -1;
    m_current.renderEndNs = -1;
    m_current.frameEndNs = -1;
    m_current.presentQueueNs = -1;
    m_current.finalized.store(false, std::memory_order_relaxed);
    m_pending.active = false;
    m_current.stage.store(InputLatencyStage::Synchronizing, std::memory_order_release);
}

bool InputLatencyTimeline::currentMatchesEpoch(InputLatencyStage stage) const
{
    return stage != InputLatencyStage::None && m_current.epoch == m_epoch.load(std::memory_order_acquire);
}

void InputLatencyTimeline::beforeSynchronizing(Nanoseconds now)
{
    const InputLatencyStage stage = m_current.stage.load(std::memory_order_acquire);
    if (stage != InputLatencyStage::None && m_current.epoch == m_epoch.load(std::memory_order_acquire))
        return;
    if (m_publicationPermits[static_cast<std::size_t>(InputLatencyRecordSlot::Current)].load(std::memory_order_acquire)
        != 0) {
        m_current.stage.store(InputLatencyStage::None, std::memory_order_release);
        return;
    }

    if (!m_pending.active || m_pending.epoch != m_epoch.load(std::memory_order_acquire)) {
        m_current.stage.store(InputLatencyStage::None, std::memory_order_release);
        return;
    }

    resetCurrentFromPending(toNanoseconds(now));
}

void InputLatencyTimeline::afterSynchronizing(Nanoseconds now)
{
    const InputLatencyStage stage = m_current.stage.load(std::memory_order_acquire);
    if (!currentMatchesEpoch(stage) || m_current.finalized.load(std::memory_order_acquire))
        return;
    m_current.syncEndNs = toNanoseconds(now);
    m_current.stage.store(InputLatencyStage::Rendering, std::memory_order_release);
}

void InputLatencyTimeline::afterRendering(Nanoseconds now)
{
    const InputLatencyStage stage = m_current.stage.load(std::memory_order_acquire);
    if (!currentMatchesEpoch(stage) || m_current.finalized.load(std::memory_order_acquire))
        return;
    m_current.renderEndNs = toNanoseconds(now);
    m_current.stage.store(InputLatencyStage::Submitted, std::memory_order_release);
}

void InputLatencyTimeline::frameSwapped(Nanoseconds now)
{
    const InputLatencyStage stage = m_current.stage.load(std::memory_order_acquire);
    if (!currentMatchesEpoch(stage) || m_current.finalized.load(std::memory_order_acquire))
        return;
    m_current.presentQueueNs = toNanoseconds(now);
    m_current.stage.store(InputLatencyStage::PresentQueued, std::memory_order_release);
}

InputLatencySample InputLatencyTimeline::sampleFromPending(qint64 terminalNs) const
{
    InputLatencySample sample;
    sample.sequence = m_pending.sequence;
    sample.epoch = m_pending.epoch;
    sample.recordSlot = InputLatencyRecordSlot::Pending;
    sample.event = m_pending.event;
    sample.coalesced = m_pending.coalesced;
    sample.budgetNs = m_pending.budgetNs;
    sample.refreshSource = m_pending.refreshSource;
    sample.totalNs = std::max<qint64>(0, terminalNs - m_pending.inputNs);
    sample.dispatchNs = m_pending.dispatchNs;
    sample.stage = InputLatencyStage::WaitingForSync;
    sample.late = sample.totalNs > sample.budgetNs;
    sample.exposed = m_pending.exposed;
    return sample;
}

InputLatencySample InputLatencyTimeline::sampleFromCurrent(
    qint64 terminalNs, InputLatencyStage publishedStage, bool completedFrame) const
{
    InputLatencySample sample;
    sample.sequence = m_current.sequence;
    sample.epoch = m_current.epoch;
    sample.recordSlot = InputLatencyRecordSlot::Current;
    sample.event = m_current.event;
    sample.coalesced = m_current.coalesced;
    sample.budgetNs = m_current.budgetNs;
    sample.refreshSource = m_current.refreshSource;
    sample.dispatchNs = m_current.dispatchNs;

    if (publishedStage >= InputLatencyStage::Synchronizing)
        sample.syncBeginNs = m_current.syncBeginNs - m_current.inputNs;
    if (publishedStage >= InputLatencyStage::Rendering)
        sample.syncEndNs = m_current.syncEndNs - m_current.inputNs;
    if (publishedStage >= InputLatencyStage::Submitted)
        sample.renderEndNs = m_current.renderEndNs - m_current.inputNs;
    if (completedFrame)
        sample.frameEndNs = m_current.frameEndNs - m_current.inputNs;
    if (publishedStage >= InputLatencyStage::PresentQueued && m_current.presentQueueNs >= 0)
        sample.presentQueueNs = m_current.presentQueueNs - m_current.inputNs;

    if (publishedStage == InputLatencyStage::PresentQueued && m_current.presentQueueNs >= 0) {
        sample.stage = InputLatencyStage::PresentQueued;
        terminalNs = m_current.presentQueueNs;
    } else if (completedFrame) {
        sample.stage = InputLatencyStage::Submitted;
        terminalNs = m_current.frameEndNs;
    } else {
        sample.stage = publishedStage;
    }

    sample.totalNs = std::max<qint64>(0, terminalNs - m_current.inputNs);
    sample.late = sample.totalNs > sample.budgetNs;
    sample.exposed = m_current.exposed;
    return sample;
}

std::optional<InputLatencySample> InputLatencyTimeline::afterFrameEnd(Nanoseconds now, bool)
{
    const InputLatencyStage stage = m_current.stage.load(std::memory_order_acquire);
    if (!currentMatchesEpoch(stage)) {
        m_current.stage.store(InputLatencyStage::None, std::memory_order_release);
        return std::nullopt;
    }

    bool expected = false;
    if (!m_current.finalized.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        m_current.stage.store(InputLatencyStage::None, std::memory_order_release);
        return std::nullopt;
    }
    m_current.frameEndNs = toNanoseconds(now);
    InputLatencySample sample = sampleFromCurrent(m_current.frameEndNs, stage, true);
    auto& permit = m_publicationPermits[static_cast<std::size_t>(InputLatencyRecordSlot::Current)];
    permit.store(sample.sequence, std::memory_order_release);
    if (sample.epoch != m_epoch.load(std::memory_order_acquire)) {
        quint64 staleSequence = sample.sequence;
        permit.compare_exchange_strong(staleSequence, 0, std::memory_order_acq_rel, std::memory_order_acquire);
        m_current.stage.store(InputLatencyStage::None, std::memory_order_release);
        return std::nullopt;
    }
    m_current.stage.store(InputLatencyStage::None, std::memory_order_release);
    return sample;
}

InputLatencyExpiredSamples InputLatencyTimeline::expire(Nanoseconds now, bool)
{
    InputLatencyExpiredSamples expired;
    const qint64 nowNs = toNanoseconds(now);
    const quint64 activeEpoch = m_epoch.load(std::memory_order_acquire);

    const InputLatencyStage stage = m_current.stage.load(std::memory_order_acquire);
    if (stage != InputLatencyStage::None && m_current.epoch == activeEpoch && nowNs > m_current.deadlineNs) {
        bool expected = false;
        if (m_current.finalized.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
            expired.samples[expired.count] = sampleFromCurrent(nowNs, stage, false);
            m_publicationPermits[static_cast<std::size_t>(InputLatencyRecordSlot::Current)].store(
                expired.samples[expired.count].sequence, std::memory_order_release);
            ++expired.count;
        }
    }

    if (m_pending.active && m_pending.epoch == activeEpoch && nowNs > m_pending.deadlineNs && !m_pending.finalized) {
        // If the scene graph never started a frame, the event did not have
        // a visual response to measure. Publishing it as input latency ties
        // the event to an arbitrary later wake-up (often hundreds of
        // milliseconds on render-on-demand webOS) and produces a false
        // stall. Route transitions separately diagnose missing UI work.
        m_pending.finalized = true;
        m_pending.active = false;
    }

    return expired;
}

std::optional<InputLatencyTimeline::Nanoseconds> InputLatencyTimeline::nextDeadline() const
{
    std::optional<qint64> deadline;
    const quint64 activeEpoch = m_epoch.load(std::memory_order_acquire);

    const InputLatencyStage stage = m_current.stage.load(std::memory_order_acquire);
    if (stage != InputLatencyStage::None && m_current.epoch == activeEpoch
        && !m_current.finalized.load(std::memory_order_acquire)) {
        deadline = m_current.deadlineNs;
    }

    if (m_pending.active && !m_pending.finalized && m_pending.epoch == activeEpoch)
        deadline = deadline ? std::min(*deadline, m_pending.deadlineNs) : m_pending.deadlineNs;

    if (!deadline)
        return std::nullopt;
    return Nanoseconds(*deadline + 1);
}

void InputLatencyTimeline::cancel()
{
    m_epoch.fetch_add(1, std::memory_order_acq_rel);
    m_pending.active = false;
    m_pending.finalized = true;
    for (auto& permit : m_publicationPermits)
        permit.store(0, std::memory_order_release);
}

void InputLatencyTimeline::clearStatistics()
{
    cancel();
    m_sampleCount = 0;
    m_lateCount = 0;
    m_missedFrameCount = 0;
    m_lastLatencyNs = 0;
    m_worstLatencyNs = 0;
    m_lastStage = InputLatencyStage::None;
}

bool InputLatencyTimeline::publish(const InputLatencySample& sample)
{
    const quint64 activeEpoch = m_epoch.load(std::memory_order_acquire);
    if (!m_enabled || sample.epoch != activeEpoch)
        return false;
    auto& permit = m_publicationPermits[static_cast<std::size_t>(sample.recordSlot)];
    quint64 expectedSequence = sample.sequence;
    if (!permit.compare_exchange_strong(expectedSequence, 0, std::memory_order_acq_rel, std::memory_order_acquire)
        || sample.epoch != m_epoch.load(std::memory_order_acquire)) {
        return false;
    }

    ++m_sampleCount;
    if (sample.late)
        ++m_lateCount;
    if (sample.late && sample.budgetNs > 0)
        m_missedFrameCount += static_cast<quint64>((sample.totalNs - 1) / sample.budgetNs);
    m_lastLatencyNs = sample.totalNs;
    m_worstLatencyNs = std::max(m_worstLatencyNs, sample.totalNs);
    m_lastStage = sample.stage;
    return true;
}

quint64 InputLatencyTimeline::sampleCount() const
{
    return m_sampleCount;
}

quint64 InputLatencyTimeline::lateCount() const
{
    return m_lateCount;
}

quint64 InputLatencyTimeline::missedFrameCount() const
{
    return m_missedFrameCount;
}

double InputLatencyTimeline::lastLatencyMs() const
{
    return toMilliseconds(m_lastLatencyNs);
}

double InputLatencyTimeline::worstLatencyMs() const
{
    return toMilliseconds(m_worstLatencyNs);
}

QString InputLatencyTimeline::lastStage() const
{
    return inputLatencyStageName(m_lastStage);
}

quint64 InputLatencyTimeline::epoch() const
{
    return m_epoch.load(std::memory_order_acquire);
}

} // namespace Spool::Detail

namespace Spool {

namespace {
    constexpr auto kEnabledSetting = "diagnostics/inputLatencyGuard";
    constexpr auto kOverlayEnabledSetting = "diagnostics/inputLatencyOverlay";

    qint64 threadCpuNowNs()
    {
#ifdef Q_OS_WIN
        return 0;
#else
        timespec value {};
        return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0
            ? static_cast<qint64>(value.tv_sec) * 1000000000LL + value.tv_nsec
            : 0;
#endif
    }

}

using Detail::InputLatencyRefreshSource;
using Detail::InputLatencySample;
using Detail::InputLatencyTimeline;

InputLatencyMonitor::InputLatencyMonitor(QObject *parent)
    : QObject(parent)
{
    m_deadlineTimer.setSingleShot(true);
    m_deadlineTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_deadlineTimer, &QChronoTimer::timeout, this, &InputLatencyMonitor::handleDeadline);

    m_warningTimer.setSingleShot(true);
    m_warningTimer.setTimerType(Qt::PreciseTimer);
    m_warningTimer.setInterval(std::chrono::seconds(2));
    connect(&m_warningTimer, &QTimer::timeout, this, &InputLatencyMonitor::hideWarning);

    m_uiGapTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_uiGapTimer, &QChronoTimer::timeout, this, &InputLatencyMonitor::handleUiGapTimer);

    const bool persistedEnabled = QSettings().value(QLatin1String(kEnabledSetting), false).toBool();
    m_overlayEnabled = QSettings().value(QLatin1String(kOverlayEnabledSetting), true).toBool();
    const bool environmentEnabled = qEnvironmentVariable("SPOOL_INPUT_LATENCY_DIAGNOSTICS") == QLatin1String("1");
    m_timeline.setEnabled(persistedEnabled || environmentEnabled);
}

InputLatencyTimeline::Nanoseconds InputLatencyMonitor::now()
{
    return std::chrono::duration_cast<InputLatencyTimeline::Nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
}

void InputLatencyMonitor::attachWindow(QQuickWindow *window)
{
    Q_ASSERT(window);
    Q_ASSERT(!m_window);
    if (!window || m_window)
        return;

    m_window = window;
    window->installEventFilter(this);
    connect(window, &QObject::destroyed, this, [this] {
        m_window = nullptr;
        cancelMeasurements();
    });
    connect(window, &QWindow::screenChanged, this, &InputLatencyMonitor::updateScreen);
    connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
        if (!visible)
            cancelMeasurements();
    });

    connect(
        window, &QQuickWindow::beforeSynchronizing, this,
        [this] {
            const qint64 timestamp = now().count();
            m_timeline.beforeSynchronizing(InputLatencyTimeline::Nanoseconds(timestamp));
            if (m_uiTransitionActive.load(std::memory_order_acquire))
                m_uiSyncBeginNs.store(timestamp, std::memory_order_release);
        },
        Qt::DirectConnection);
    connect(
        window, &QQuickWindow::afterSynchronizing, this,
        [this] {
            const qint64 timestamp = now().count();
            m_timeline.afterSynchronizing(InputLatencyTimeline::Nanoseconds(timestamp));
            if (m_uiTransitionActive.load(std::memory_order_acquire)) {
                const qint64 begin = m_uiSyncBeginNs.exchange(-1, std::memory_order_acq_rel);
                if (begin >= 0)
                    m_uiSyncNs.fetch_add(std::max<qint64>(0, timestamp - begin));
                m_uiRenderBeginNs.store(timestamp, std::memory_order_release);
            }
        },
        Qt::DirectConnection);
    connect(
        window, &QQuickWindow::afterRendering, this,
        [this] {
            const qint64 timestamp = now().count();
            m_timeline.afterRendering(InputLatencyTimeline::Nanoseconds(timestamp));
            if (m_uiTransitionActive.load(std::memory_order_acquire)) {
                const qint64 begin = m_uiRenderBeginNs.exchange(-1, std::memory_order_acq_rel);
                if (begin >= 0)
                    m_uiRenderNs.fetch_add(std::max<qint64>(0, timestamp - begin));
                m_uiRenderBeginNs.store(timestamp, std::memory_order_release);
            }
        },
        Qt::DirectConnection);
    connect(
        window, &QQuickWindow::frameSwapped, this,
        [this] {
            m_hasPresentedFrame.store(true, std::memory_order_release);
            m_timeline.frameSwapped(now());
        },
        Qt::DirectConnection);
    connect(
        window, &QQuickWindow::frameSwapped, this,
        [this] {
            if (!m_uiTransitionActive.load(std::memory_order_acquire))
                return;
            const qint64 frameNs = now().count();
            m_uiActualSwaps.fetch_add(1, std::memory_order_relaxed);
            const qint64 renderEnd = m_uiRenderBeginNs.exchange(-1, std::memory_order_acq_rel);
            if (renderEnd >= 0)
                m_uiSwapWaitNs.fetch_add(std::max<qint64>(0, frameNs - renderEnd));
            QMetaObject::invokeMethod(
                this, [this, frameNs] { handleUiTransitionFrame(frameNs); }, Qt::QueuedConnection);
        },
        Qt::DirectConnection);
    connect(
        window, &QQuickWindow::afterFrameEnd, this,
        [this] {
            const auto sample = m_timeline.afterFrameEnd(now(), true);
            if (!sample)
                return;
            QMetaObject::invokeMethod(
                this,
                [this, sample = *sample] {
                    handleCompletedSample(sample);
                    scheduleDeadline();
                },
                Qt::QueuedConnection);
        },
        Qt::DirectConnection);
    connect(
        window, &QQuickWindow::sceneGraphInvalidated, this,
        [this] {
            m_timeline.cancel();
            QMetaObject::invokeMethod(this, &InputLatencyMonitor::finishCancellationOnGuiThread, Qt::QueuedConnection);
        },
        Qt::DirectConnection);

    if (auto *application = qGuiApp) {
        connect(application, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationHidden || state == Qt::ApplicationSuspended)
                cancelMeasurements();
        });
    }

    updateScreen(window->screen());
}

quint64 InputLatencyMonitor::beginInput(const QEvent *event)
{
    if (!canCaptureInput())
        return 0;
    const auto metadata = Detail::classifyInputEvent(event, event && event->spontaneous());
    if (!metadata)
        return 0;
    const auto timestamp = now();
    const quint64 token = m_timeline.beginInput(*metadata, timestamp,
        InputLatencyTimeline::Nanoseconds(m_frameBudgetNs), m_refreshSource, m_window->isExposed());
    if (token != 0)
        m_lastInputBeginNs = timestamp.count();
    return token;
}

void InputLatencyMonitor::endInput(quint64 token)
{
    if (token == 0)
        return;
    m_timeline.endInput(token, now());
    // No forced window update here: forcing a render per input event cost a
    // full frame of GPU work per key (and queued behind in-flight frames,
    // inflating every sample by ~2 vsyncs). Events that change the scene
    // produce frames on their own; no-op events expire via the deadline.
    scheduleDeadline();
}

void InputLatencyMonitor::UiTransition::reset()
{
    const quint64 previousSequence = sequence;
    *this = UiTransition {};
    sequence = previousSequence;
}

quint64 InputLatencyMonitor::beginUiTransition(
    const QString& name, const QString& routeFrom, const QString& routeTo, const QString& cacheHit)
{
    if (!canCaptureInput() || name.isEmpty())
        return 0;

    resetUiTransition();
    m_uiTransition.token = ++m_uiTransition.sequence;
    m_uiTransition.name = name;
    m_uiTransition.routeFrom = routeFrom;
    m_uiTransition.routeTo = routeTo;
    m_uiTransition.cacheHit = cacheHit;
    m_uiTransition.beginNs = now().count();
    m_uiTransition.beginCpuNs = threadCpuNowNs();
    if (m_lastInputBeginNs >= 0 && m_uiTransition.beginNs - m_lastInputBeginNs <= m_frameBudgetNs * 2)
        m_uiTransition.inputBeginNs = m_lastInputBeginNs;
    m_uiActualSwaps.store(0, std::memory_order_relaxed);
    m_uiSyncNs.store(0, std::memory_order_relaxed);
    m_uiRenderNs.store(0, std::memory_order_relaxed);
    m_uiSwapWaitNs.store(0, std::memory_order_relaxed);
    m_uiTransitionActive.store(true, std::memory_order_release);
    m_uiTransition.expectedGapFireNs = m_uiTransition.beginNs + m_frameBudgetNs;
    m_uiGapTimer.setInterval(std::chrono::nanoseconds(m_frameBudgetNs));
    m_uiGapTimer.start();
    return m_uiTransition.token;
}

void InputLatencyMonitor::mark(quint64 token, const QString& stage)
{
    std::optional<UiTransition::Stage> parsed;
    if (stage == QLatin1String("instance"))
        parsed = UiTransition::Stage::Instance;
    else if (stage == QLatin1String("shell"))
        parsed = UiTransition::Stage::Shell;
    else if (stage == QLatin1String("model_ready"))
        parsed = UiTransition::Stage::ModelReady;
    else if (stage == QLatin1String("first_delegate"))
        parsed = UiTransition::Stage::FirstDelegate;
    else if (stage == QLatin1String("viewport"))
        parsed = UiTransition::Stage::Viewport;
    else if (stage == QLatin1String("content_ready"))
        parsed = UiTransition::Stage::ContentReady;
    if (!parsed || token == 0 || token != m_uiTransition.token)
        return;
    auto& destination = m_uiTransition.marks[static_cast<std::size_t>(*parsed)];
    if (destination.elapsedNs >= 0)
        return;
    destination.elapsedNs = std::max<qint64>(0, now().count() - m_uiTransition.beginNs);
    destination.cpuNs = std::max<qint64>(0, threadCpuNowNs() - m_uiTransition.beginCpuNs);
    if (m_window)
        m_window->update();
}

void InputLatencyMonitor::noteDelegate(const QString& kind, int delta)
{
    if (!m_uiTransitionActive.load(std::memory_order_acquire) || delta == 0)
        return;
    m_uiTransition.delegateCounts[kind] += delta;
    if (delta > 0)
        m_uiTransition.delegatesCreated += static_cast<quint64>(delta);
    else
        m_uiTransition.delegatesDestroyed += static_cast<quint64>(-delta);
}

void InputLatencyMonitor::handleUiTransitionFrame(qint64 frameNs)
{
    const auto& ready = m_uiTransition.marks[static_cast<std::size_t>(UiTransition::Stage::ContentReady)];
    if (m_uiTransition.token == 0 || ready.elapsedNs < 0 || frameNs < m_uiTransition.beginNs + ready.elapsedNs)
        return;

    Detail::UiLatencySample sample;
    sample.name = m_uiTransition.name;
    sample.routeFrom = m_uiTransition.routeFrom;
    sample.routeTo = m_uiTransition.routeTo;
    sample.cacheHit = m_uiTransition.cacheHit;
    sample.budgetNs = m_frameBudgetNs;
    sample.totalNs = std::max<qint64>(0, frameNs - m_uiTransition.beginNs);
    sample.guiCpuNs = std::max<qint64>(0, threadCpuNowNs() - m_uiTransition.beginCpuNs);
    sample.inputNs = m_uiTransition.inputBeginNs < 0 ? -1 : frameNs - m_uiTransition.inputBeginNs;
    sample.presentNs = std::max<qint64>(0, sample.totalNs - ready.elapsedNs);
    sample.syncNs = m_uiSyncNs.load(std::memory_order_acquire);
    sample.renderNs = m_uiRenderNs.load(std::memory_order_acquire);
    sample.swapWaitNs = m_uiSwapWaitNs.load(std::memory_order_acquire);
    sample.maxGapNs = m_uiTransition.maxGapNs;
    sample.budgetIntervals
        = static_cast<quint64>(std::max<qint64>(1, (sample.totalNs + m_frameBudgetNs - 1) / m_frameBudgetNs));
    sample.actualSwaps = m_uiActualSwaps.load(std::memory_order_acquire);
    sample.delegatesCreated = m_uiTransition.delegatesCreated;
    sample.delegatesDestroyed = m_uiTransition.delegatesDestroyed;
    for (std::size_t index = 0; index < sample.stageNs.size(); ++index)
        sample.stageNs[index] = m_uiTransition.marks[index].elapsedNs;

    const QString line = Detail::formatUiLatency(sample);
    const auto ms = [](qint64 nanoseconds) { return static_cast<double>(nanoseconds) / 1000000.0; };
    m_lastRouteMetrics = QVariantMap {
        { QStringLiteral("name"), sample.name },
        { QStringLiteral("routeFrom"), sample.routeFrom },
        { QStringLiteral("routeTo"), sample.routeTo },
        { QStringLiteral("cacheHit"), sample.cacheHit },
        { QStringLiteral("wallMs"), ms(sample.totalNs) },
        { QStringLiteral("guiCpuMs"), ms(sample.guiCpuNs) },
        { QStringLiteral("presentMs"), ms(sample.presentNs) },
        { QStringLiteral("syncMs"), ms(sample.syncNs) },
        { QStringLiteral("renderMs"), ms(sample.renderNs) },
        { QStringLiteral("swapWaitMs"), ms(sample.swapWaitNs) },
        { QStringLiteral("maxGapMs"), ms(sample.maxGapNs) },
        { QStringLiteral("frameBudgetMs"), ms(sample.budgetNs) },
        { QStringLiteral("budgetIntervals"), sample.budgetIntervals },
        { QStringLiteral("actualSwaps"), sample.actualSwaps },
        { QStringLiteral("delegatesCreated"), sample.delegatesCreated },
        { QStringLiteral("delegatesDestroyed"), sample.delegatesDestroyed },
        { QStringLiteral("instanceMs"), ms(sample.stageNs[0]) },
        { QStringLiteral("shellMs"), ms(sample.stageNs[1]) },
        { QStringLiteral("modelReadyMs"), ms(sample.stageNs[2]) },
        { QStringLiteral("firstDelegateMs"), ms(sample.stageNs[3]) },
        { QStringLiteral("viewportMs"), ms(sample.stageNs[4]) },
        { QStringLiteral("contentReadyMs"), ms(sample.stageNs[5]) },
    };
    const auto milliseconds
        = [](qint64 nanoseconds) { return QString::number(static_cast<double>(nanoseconds) / 1000000.0, 'f', 2); };
    m_lastRouteSample = QStringLiteral("%1  %2 ms / %3 ms CPU  %4 swaps  %5+/%6- delegates")
                            .arg(sample.name, milliseconds(sample.totalNs), milliseconds(sample.guiCpuNs))
                            .arg(sample.actualSwaps)
                            .arg(sample.delegatesCreated)
                            .arg(sample.delegatesDestroyed);
    emit routeSampleChanged();
    resetUiTransition();
    if (sample.totalNs <= sample.budgetNs * 2) {
        qInfo().noquote() << line;
        return;
    }

    qWarning().noquote() << line;
    const bool replaceWarning = !m_warningVisible || sample.totalNs > m_warningLatencyNs;
    if (replaceWarning) {
        m_warningVisible = true;
        m_warningLatencyNs = sample.totalNs;
        m_warningStage = QStringLiteral("content_presented");
        m_warningText = QStringLiteral("UI content took %1 swaps: %2").arg(sample.actualSwaps).arg(sample.name);
        emit warningChanged();
    }
    m_warningTimer.start();
}

bool InputLatencyMonitor::enabled() const
{
    return m_timeline.enabled();
}

void InputLatencyMonitor::setEnabled(bool enabled)
{
    if (m_timeline.enabled() == enabled)
        return;
    m_timeline.setEnabled(enabled);
    QSettings().setValue(QLatin1String(kEnabledSetting), enabled);
    finishCancellationOnGuiThread();
    emit enabledChanged();
}

bool InputLatencyMonitor::overlayEnabled() const
{
    return m_overlayEnabled;
}

void InputLatencyMonitor::setOverlayEnabled(bool enabled)
{
    if (m_overlayEnabled == enabled)
        return;
    m_overlayEnabled = enabled;
    QSettings().setValue(QLatin1String(kOverlayEnabledSetting), enabled);
    emit overlayEnabledChanged();
}

bool InputLatencyMonitor::warningVisible() const
{
    return m_warningVisible;
}

QString InputLatencyMonitor::warningText() const
{
    return m_warningText;
}

QString InputLatencyMonitor::warningStage() const
{
    return m_warningStage;
}

double InputLatencyMonitor::lastLatencyMs() const
{
    return m_timeline.lastLatencyMs();
}

double InputLatencyMonitor::worstLatencyMs() const
{
    return m_timeline.worstLatencyMs();
}

QString InputLatencyMonitor::lastStage() const
{
    return m_timeline.lastStage();
}

quint64 InputLatencyMonitor::sampleCount() const
{
    return m_timeline.sampleCount();
}

quint64 InputLatencyMonitor::lateCount() const
{
    return m_timeline.lateCount();
}

quint64 InputLatencyMonitor::missedFrameCount() const
{
    return m_timeline.missedFrameCount();
}

double InputLatencyMonitor::frameBudgetMs() const
{
    return static_cast<double>(m_frameBudgetNs) / 1000000.0;
}

QString InputLatencyMonitor::lastRouteSample() const
{
    return m_lastRouteSample;
}

QVariantMap InputLatencyMonitor::lastRouteMetrics() const
{
    return m_lastRouteMetrics;
}

void InputLatencyMonitor::handleUiGapTimer()
{
    if (m_uiTransition.token == 0)
        return;
    const qint64 timestamp = now().count();
    m_uiTransition.maxGapNs
        = std::max(m_uiTransition.maxGapNs, std::max<qint64>(0, timestamp - m_uiTransition.expectedGapFireNs));
    m_uiTransition.expectedGapFireNs = timestamp + m_frameBudgetNs;
}

void InputLatencyMonitor::resetUiTransition()
{
    m_uiTransitionActive.store(false, std::memory_order_release);
    m_uiGapTimer.stop();
    m_uiTransition.reset();
    m_uiSyncBeginNs.store(-1, std::memory_order_relaxed);
    m_uiRenderBeginNs.store(-1, std::memory_order_relaxed);
}

void InputLatencyMonitor::clearStatistics()
{
    m_timeline.clearStatistics();
    finishCancellationOnGuiThread();
    emit statisticsChanged();
}

void InputLatencyMonitor::updateScreen(QScreen *screen)
{
    if (m_screen == screen) {
        updateFrameBudget();
        return;
    }
    disconnect(m_refreshRateConnection);
    m_screen = screen;
    if (m_screen) {
        m_refreshRateConnection
            = connect(m_screen, &QScreen::refreshRateChanged, this, [this] { updateFrameBudget(); });
    }
    updateFrameBudget();
}

bool InputLatencyMonitor::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_window
        && (event->type() == QEvent::Hide || event->type() == QEvent::Close
            || (event->type() == QEvent::Expose && !m_window->isExposed()))) {
        cancelMeasurements();
    }
    return QObject::eventFilter(watched, event);
}

void InputLatencyMonitor::updateFrameBudget()
{
    qint64 budgetNs = 16670000;
    InputLatencyRefreshSource source = InputLatencyRefreshSource::Fallback60;
    if (m_screen) {
        const qreal refreshRate = m_screen->refreshRate();
        if (std::isfinite(refreshRate) && refreshRate >= 30.0 && refreshRate <= 240.0) {
            budgetNs = std::llround(1000000000.0 / refreshRate);
            source = InputLatencyRefreshSource::Screen;
        }
    }
    if (m_frameBudgetNs == budgetNs && m_refreshSource == source)
        return;
    m_frameBudgetNs = budgetNs;
    m_refreshSource = source;
    emit frameBudgetChanged();
}

void InputLatencyMonitor::scheduleDeadline()
{
    const auto deadline = m_timeline.nextDeadline();
    if (!deadline) {
        m_deadlineTimer.stop();
        return;
    }
    const auto delay = std::max(InputLatencyTimeline::Nanoseconds::zero(), *deadline - now());
    m_deadlineTimer.setInterval(delay);
    m_deadlineTimer.start();
}

void InputLatencyMonitor::handleDeadline()
{
    const auto expired = m_timeline.expire(now(), m_window && m_window->isExposed());
    for (std::size_t index = 0; index < expired.count; ++index)
        handleCompletedSample(expired.samples[index]);
    scheduleDeadline();
}

void InputLatencyMonitor::handleCompletedSample(const InputLatencySample& sample)
{
    if (!m_timeline.publish(sample))
        return;

    emit statisticsChanged();
    // Continuous pointer motion can overlap video-paced frames without the
    // pointer dispatch itself being slow. Retain those samples in statistics,
    // but reserve warnings for a visible 100 ms hover stall.
    if (!Detail::shouldWarnInputLatency(sample))
        return;

    qWarning().noquote() << Detail::formatInputLatencyMiss(sample);
    const bool replaceWarning = !m_warningVisible || sample.totalNs > m_warningLatencyNs;
    if (replaceWarning) {
        m_warningVisible = true;
        m_warningLatencyNs = sample.totalNs;
        m_warningStage = Detail::inputLatencyStageName(sample.stage);
        m_warningText = QStringLiteral("Input response missed frame budget: %1 ms / %2 ms")
                            .arg(QString::number(static_cast<double>(sample.totalNs) / 1000000.0, 'f', 2),
                                QString::number(static_cast<double>(sample.budgetNs) / 1000000.0, 'f', 2));
        emit warningChanged();
    }
    m_warningTimer.start();
}

void InputLatencyMonitor::cancelMeasurements()
{
    m_timeline.cancel();
    resetUiTransition();
    finishCancellationOnGuiThread();
}

void InputLatencyMonitor::finishCancellationOnGuiThread()
{
    m_deadlineTimer.stop();
    hideWarning();
}

void InputLatencyMonitor::hideWarning()
{
    m_warningTimer.stop();
    if (!m_warningVisible && m_warningText.isEmpty() && m_warningStage.isEmpty())
        return;
    m_warningVisible = false;
    m_warningText.clear();
    m_warningStage.clear();
    m_warningLatencyNs = 0;
    emit warningChanged();
}

bool InputLatencyMonitor::canCaptureInput() const
{
    if (!m_timeline.enabled() || !m_hasPresentedFrame.load(std::memory_order_acquire) || !m_window
        || !m_window->isVisible() || !m_window->isExposed())
        return false;
    if (const auto *application = qGuiApp) {
        const Qt::ApplicationState state = application->applicationState();
        if (state == Qt::ApplicationHidden || state == Qt::ApplicationSuspended)
            return false;
    }
    return true;
}

} // namespace Spool
