#include "diagnostics/InputLatencyMonitor.h"

#include "TestMain.h"

#include <QDebug>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QStringList>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>

using Spool::Detail::classifyInputEvent;
using Spool::Detail::formatInputLatencyMiss;
using Spool::Detail::formatUiLatency;
using Spool::Detail::InputLatencyEventKind;
using Spool::Detail::InputLatencyEventMetadata;
using Spool::Detail::InputLatencyExpiredSamples;
using Spool::Detail::InputLatencyRefreshSource;
using Spool::Detail::InputLatencySample;
using Spool::Detail::InputLatencyStage;
using Spool::Detail::InputLatencyTimeline;
using Spool::Detail::shouldWarnInputLatency;
using Spool::Detail::UiLatencySample;

namespace {

using Nanoseconds = InputLatencyTimeline::Nanoseconds;

constexpr Nanoseconds ns(qint64 value)
{
    return Nanoseconds(value);
}

constexpr Nanoseconds ms(qint64 value)
{
    return Nanoseconds(value * 1'000'000);
}

void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(1);
}

InputLatencyEventMetadata keyEvent(int key = Qt::Key_Right, quint32 scanCode = 106)
{
    return { InputLatencyEventKind::KeyPress, key, scanCode };
}

void enable(InputLatencyTimeline& timeline)
{
    timeline.setEnabled(true);
    require(timeline.enabled(), "timeline should be enabled");
}

InputLatencySample completeSubmitted(InputLatencyTimeline& timeline, Nanoseconds frameEnd, bool exposed = true)
{
    const std::optional<InputLatencySample> sample = timeline.afterFrameEnd(frameEnd, exposed);
    require(sample.has_value(), "submitted frame should produce a sample");
    return *sample;
}

void testDisabledCapture()
{
    InputLatencyTimeline timeline;
    require(!timeline.enabled(), "timeline should default disabled");
    require(timeline.beginInput(keyEvent(), ns(1), ms(16), InputLatencyRefreshSource::Fallback60, true) == 0,
        "disabled capture should return token zero");
    timeline.endInput(0, ns(2));
    timeline.beforeSynchronizing(ns(3));
    require(!timeline.afterFrameEnd(ns(4), true), "disabled input should not attach to a frame");
    require(timeline.expire(ms(20), true).count == 0, "disabled input should not expire");
    require(!timeline.nextDeadline(), "disabled input should not schedule a deadline");
}

void testEventClassifier()
{
    require(!classifyInputEvent(nullptr, true), "null event should be excluded");

    QKeyEvent keyPress(
        QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, 0x34, 0x41, 0, QStringLiteral("private typed text"));
    require(!classifyInputEvent(&keyPress, false), "non-spontaneous key event should be excluded");
    const auto classifiedKey = classifyInputEvent(&keyPress, true);
    require(classifiedKey && classifiedKey->kind == InputLatencyEventKind::KeyPress,
        "spontaneous key press should be included");
    require(classifiedKey->key == Qt::Key_A && classifiedKey->nativeScanCode == 0x34,
        "key classifier should retain code and native scan code");

    QKeyEvent keyRelease(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier);
    const auto classifiedRelease = classifyInputEvent(&keyRelease, true);
    require(classifiedRelease && classifiedRelease->kind == InputLatencyEventKind::KeyRelease,
        "spontaneous key release should be included");

    struct EventCase {
        QEvent::Type type;
        InputLatencyEventKind kind;
    };
    const EventCase eventCases[] = {
        { QEvent::Wheel, InputLatencyEventKind::Wheel },
        { QEvent::TouchBegin, InputLatencyEventKind::TouchBegin },
        { QEvent::TouchUpdate, InputLatencyEventKind::TouchUpdate },
        { QEvent::TouchEnd, InputLatencyEventKind::TouchEnd },
        { QEvent::TouchCancel, InputLatencyEventKind::TouchCancel },
        { QEvent::TabletPress, InputLatencyEventKind::TabletPress },
        { QEvent::TabletMove, InputLatencyEventKind::TabletMove },
        { QEvent::TabletRelease, InputLatencyEventKind::TabletRelease },
    };
    for (const EventCase& eventCase : eventCases) {
        QEvent event(eventCase.type);
        const auto classified = classifyInputEvent(&event, true);
        require(classified && classified->kind == eventCase.kind, "tracked input class should be included");
    }

    QPointingDevice mouseDevice(QStringLiteral("mouse"), 1, QInputDevice::DeviceType::Mouse,
        QPointingDevice::PointerType::Generic, QInputDevice::Capability::Position, 1, 3);
    const EventCase mouseCases[] = {
        { QEvent::MouseButtonPress, InputLatencyEventKind::MousePress },
        { QEvent::MouseButtonRelease, InputLatencyEventKind::MouseRelease },
        { QEvent::MouseButtonDblClick, InputLatencyEventKind::MouseDoubleClick },
        { QEvent::MouseMove, InputLatencyEventKind::MouseMove },
    };
    for (const EventCase& eventCase : mouseCases) {
        const Qt::MouseButton button = eventCase.type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent event(eventCase.type, QPointF(1, 2), QPointF(10, 20), button, button, Qt::NoModifier, &mouseDevice);
        const auto classified = classifyInputEvent(&event, true);
        require(classified && classified->kind == eventCase.kind, "physical mouse event should be included");
    }

    QPointingDevice touchDevice(QStringLiteral("touch"), 2, QInputDevice::DeviceType::TouchScreen,
        QPointingDevice::PointerType::Finger, QInputDevice::Capability::Position, 10, 0);
    QMouseEvent synthesizedMouse(QEvent::MouseButtonPress, QPointF(1, 2), QPointF(10, 20), Qt::LeftButton,
        Qt::LeftButton, Qt::NoModifier, &touchDevice);
    require(!classifyInputEvent(&synthesizedMouse, true), "mouse event derived from a touchscreen should be excluded");

    QEvent unrelated(QEvent::Resize);
    require(!classifyInputEvent(&unrelated, true), "non-input event should be excluded");
}

void testCoalescingAndFirstDispatch()
{
    InputLatencyTimeline timeline;
    enable(timeline);
    const quint64 first
        = timeline.beginInput(keyEvent(Qt::Key_Left, 105), ms(100), ms(20), InputLatencyRefreshSource::Screen, true);
    const quint64 second = timeline.beginInput(
        { InputLatencyEventKind::Wheel, 0, 0 }, ms(102), ms(20), InputLatencyRefreshSource::Screen, true);
    require(first != 0 && second > first, "captured inputs should receive monotonic tokens");

    timeline.endInput(second, ms(103));
    timeline.endInput(first, ms(104));
    timeline.beforeSynchronizing(ms(105));
    timeline.afterSynchronizing(ms(106));
    timeline.afterRendering(ms(108));
    timeline.frameSwapped(ms(110));
    const InputLatencySample sample = completeSubmitted(timeline, ms(111));

    require(sample.event.kind == InputLatencyEventKind::KeyPress && sample.event.key == Qt::Key_Left,
        "coalescing should retain oldest event metadata");
    require(sample.coalesced == 1, "coalescing should count each additional input");
    require(sample.dispatchNs == ms(4).count(), "only the first token should set dispatch duration");
    require(sample.syncBeginNs == ms(5).count(), "stage timing should remain relative to oldest input");
    require(sample.totalNs == ms(10).count(), "total should remain relative to oldest input");
    require(sample.stage == InputLatencyStage::PresentQueued, "swap should be the terminal stage");
}

void testStalePreInputFrames()
{
    InputLatencyTimeline timeline;
    enable(timeline);
    timeline.beforeSynchronizing(ms(1));
    timeline.afterSynchronizing(ms(2));
    timeline.afterRendering(ms(3));
    timeline.frameSwapped(ms(4));
    require(!timeline.afterFrameEnd(ms(5), true), "frame before input should not produce a sample");

    timeline.beginInput(keyEvent(), ms(10), ms(16), InputLatencyRefreshSource::Fallback60, true);
    timeline.beforeSynchronizing(ms(11));
    timeline.afterRendering(ms(12));
    const InputLatencySample sample = completeSubmitted(timeline, ms(13));
    require(sample.totalNs == ms(3).count(), "pre-input frame timestamps must not contaminate sample");
    require(sample.stage == InputLatencyStage::Submitted, "unswapped completed frame should be submitted");
}

void testInputDuringRendering()
{
    InputLatencyTimeline timeline;
    enable(timeline);
    timeline.beginInput(keyEvent(Qt::Key_Up, 103), ms(0), ms(20), InputLatencyRefreshSource::Screen, true);
    timeline.beforeSynchronizing(ms(1));
    timeline.afterSynchronizing(ms(2));

    timeline.beginInput(keyEvent(Qt::Key_Down, 108), ms(3), ms(20), InputLatencyRefreshSource::Screen, true);
    timeline.afterRendering(ms(4));
    timeline.frameSwapped(ms(5));
    const InputLatencySample first = completeSubmitted(timeline, ms(6));
    require(first.event.key == Qt::Key_Up && first.totalNs == ms(5).count(),
        "input during rendering must not replace the current frame batch");
    timeline.beforeSynchronizing(ms(6) + ns(1));
    require(!timeline.afterFrameEnd(ms(6) + ns(2), true),
        "current slot must not be reused before its queued publication is consumed");
    require(timeline.publish(first), "completed current batch should publish before the next latch");

    timeline.beforeSynchronizing(ms(7));
    timeline.afterRendering(ms(8));
    const InputLatencySample second = completeSubmitted(timeline, ms(9));
    require(second.event.key == Qt::Key_Down && second.syncBeginNs == ms(4).count(),
        "input during rendering should latch only on the following frame");
}

void testIndependentDeadlines()
{
    InputLatencyTimeline timeline;
    enable(timeline);
    timeline.beginInput(keyEvent(Qt::Key_Up), ms(0), ms(10), InputLatencyRefreshSource::Screen, true);
    timeline.beforeSynchronizing(ms(1));
    timeline.beginInput(keyEvent(Qt::Key_Down), ms(5), ms(20), InputLatencyRefreshSource::Screen, true);

    require(timeline.nextDeadline() == std::optional<Nanoseconds>(ms(10) + ns(1)),
        "earliest current deadline should be selected");
    require(timeline.expire(ms(10), true).count == 0, "deadline boundary should not expire early");
    const InputLatencyExpiredSamples current = timeline.expire(ms(10) + ns(1), true);
    require(current.count == 1 && current.samples[0].event.key == Qt::Key_Up,
        "current batch should expire independently first");
    require(timeline.nextDeadline() == std::optional<Nanoseconds>(ms(25) + ns(1)),
        "pending deadline should remain after current finalization");
    const InputLatencyExpiredSamples pending = timeline.expire(ms(25) + ns(1), true);
    require(pending.count == 0, "input without a scene-graph frame should expire silently");
    require(!timeline.nextDeadline(), "all finalized deadlines should be removed");
}

void testExactPeriodBoundary()
{
    constexpr Nanoseconds budget(16'670'000);
    InputLatencyTimeline timeline;
    enable(timeline);
    timeline.beginInput(keyEvent(), ns(0), budget, InputLatencyRefreshSource::Fallback60, true);
    timeline.beforeSynchronizing(ms(1));
    timeline.afterRendering(ms(10));
    const InputLatencySample exact = completeSubmitted(timeline, budget);
    require(exact.totalNs == budget.count() && !exact.late,
        "exactly one period should be on time under the strict greater-than policy");

    timeline.beginInput(keyEvent(), ms(20), budget, InputLatencyRefreshSource::Fallback60, true);
    require(timeline.expire(ms(20) + budget, true).count == 0,
        "pending sample should remain unresolved exactly at its deadline");
    const InputLatencyExpiredSamples over = timeline.expire(ms(20) + budget + ns(1), true);
    require(over.count == 0 && !timeline.nextDeadline(), "input without a rendered frame should not become a sample");
}

InputLatencySample expireAtStage(InputLatencyStage expected)
{
    InputLatencyTimeline timeline;
    enable(timeline);
    timeline.beginInput(keyEvent(), ns(0), ns(10), InputLatencyRefreshSource::Fallback60, true);
    if (expected >= InputLatencyStage::Synchronizing)
        timeline.beforeSynchronizing(ns(1));
    if (expected >= InputLatencyStage::Rendering)
        timeline.afterSynchronizing(ns(2));
    if (expected >= InputLatencyStage::Submitted)
        timeline.afterRendering(ns(3));
    if (expected >= InputLatencyStage::PresentQueued)
        timeline.frameSwapped(ns(4));
    const InputLatencyExpiredSamples expired = timeline.expire(ns(11), true);
    require(expired.count == 1, "due sample should expire exactly once");
    require(expired.samples[0].stage == expected, "expiry should report highest published stage");
    return expired.samples[0];
}

void testEveryTerminalStage()
{
    InputLatencyTimeline waiting;
    enable(waiting);
    waiting.beginInput(keyEvent(), ns(0), ns(10), InputLatencyRefreshSource::Fallback60, true);
    require(waiting.expire(ns(11), true).count == 0,
        "waiting-for-sync input should be discarded when no visual frame starts");
    const InputLatencySample synchronizing = expireAtStage(InputLatencyStage::Synchronizing);
    require(synchronizing.syncBeginNs == 1 && synchronizing.syncEndNs < 0,
        "synchronizing sample should include only sync begin");
    const InputLatencySample rendering = expireAtStage(InputLatencyStage::Rendering);
    require(rendering.syncEndNs == 2 && rendering.renderEndNs < 0,
        "rendering sample should include sync end but not render end");
    const InputLatencySample submitted = expireAtStage(InputLatencyStage::Submitted);
    require(submitted.renderEndNs == 3 && submitted.presentQueueNs < 0,
        "submitted sample should include render end but omit presentation queue");
    const InputLatencySample presented = expireAtStage(InputLatencyStage::PresentQueued);
    require(presented.presentQueueNs == 4 && presented.totalNs == 4,
        "present-queued sample should terminate at the published swap timestamp");

    InputLatencyTimeline completed;
    enable(completed);
    completed.beginInput(keyEvent(), ns(0), ns(10), InputLatencyRefreshSource::Fallback60, true);
    completed.beforeSynchronizing(ns(1));
    completed.afterRendering(ns(3));
    const InputLatencySample frameEnd = completeSubmitted(completed, ns(4));
    require(frameEnd.stage == InputLatencyStage::Submitted && frameEnd.frameEndNs == 4,
        "completed frame without swap should terminate as submitted");
}

void testEpochCancellationAndSemantics()
{
    InputLatencyTimeline timeline;
    enable(timeline);
    const quint64 enabledEpoch = timeline.epoch();
    timeline.beginInput(keyEvent(), ns(0), ns(10), InputLatencyRefreshSource::Fallback60, true);
    timeline.beforeSynchronizing(ns(1));
    timeline.cancel();
    require(timeline.epoch() == enabledEpoch + 1, "cancellation should advance epoch");
    require(timeline.expire(ns(20), false).count == 0, "cancelled current sample should not warn");
    require(!timeline.afterFrameEnd(ns(21), false), "cancelled current completion should be discarded");

    timeline.beginInput(keyEvent(), ns(30), ns(10), InputLatencyRefreshSource::Fallback60, true);
    const quint64 hideEpoch = timeline.epoch();
    timeline.cancel();
    require(timeline.epoch() == hideEpoch + 1 && timeline.expire(ns(50), false).count == 0,
        "hide-style cancellation should silently discard pending input");

    timeline.beginInput(keyEvent(), ns(60), ns(10), InputLatencyRefreshSource::Fallback60, true);
    const quint64 invalidationEpoch = timeline.epoch();
    timeline.cancel();
    require(timeline.epoch() == invalidationEpoch + 1 && !timeline.nextDeadline(),
        "scene-graph invalidation-style cancellation should remove deadlines");

    timeline.setEnabled(false);
    require(!timeline.enabled(), "disable should change enabled state");
    require(timeline.beginInput(keyEvent(), ns(70), ns(10), InputLatencyRefreshSource::Fallback60, true) == 0,
        "disable should prevent subsequent capture");
    require(timeline.expire(ns(90), true).count == 0, "disable should leave no publishable work");
}

void testDeadlineCompletionRaces()
{
    InputLatencyTimeline deadlineFirst;
    enable(deadlineFirst);
    deadlineFirst.beginInput(keyEvent(), ns(0), ns(10), InputLatencyRefreshSource::Fallback60, true);
    deadlineFirst.beforeSynchronizing(ns(1));
    const InputLatencyExpiredSamples expired = deadlineFirst.expire(ns(11), true);
    require(expired.count == 1, "deadline-first race should produce one sample");
    require(deadlineFirst.publish(expired.samples[0]), "deadline sample should publish");
    require(!deadlineFirst.publish(expired.samples[0]), "publication permit should be consumed exactly once");
    require(!deadlineFirst.afterFrameEnd(ns(12), true), "completion after deadline should lose finalization race");
    require(deadlineFirst.sampleCount() == 1, "deadline-first race should update statistics once");

    InputLatencyTimeline completionFirst;
    enable(completionFirst);
    completionFirst.beginInput(keyEvent(), ns(0), ns(10), InputLatencyRefreshSource::Fallback60, true);
    completionFirst.beforeSynchronizing(ns(1));
    const InputLatencySample completed = completeSubmitted(completionFirst, ns(11));
    require(completionFirst.publish(completed), "completion sample should publish");
    require(completionFirst.expire(ns(12), true).count == 0, "deadline after completion should lose finalization race");
    require(completionFirst.sampleCount() == 1, "completion-first race should update statistics once");

    InputLatencyTimeline stale;
    enable(stale);
    stale.beginInput(keyEvent(), ns(0), ns(10), InputLatencyRefreshSource::Fallback60, true);
    stale.beforeSynchronizing(ns(1));
    const InputLatencyExpiredSamples staleExpired = stale.expire(ns(11), true);
    stale.cancel();
    require(!stale.publish(staleExpired.samples[0]), "queued sample from an old epoch should not publish");
    require(stale.sampleCount() == 0, "stale publication should not alter statistics");
    stale.beginInput(keyEvent(Qt::Key_Right), ns(20), ns(10), InputLatencyRefreshSource::Fallback60, true);
    stale.beforeSynchronizing(ns(21));
    stale.afterRendering(ns(22));
    require(stale.afterFrameEnd(ns(23), true).has_value(),
        "cancellation should clear stale permits so the current slot can be reused");
}

QStringList capturedWarnings;

void captureWarnings(QtMsgType type, const QMessageLogContext&, const QString& message)
{
    if (type == QtWarningMsg)
        capturedWarnings.append(message);
}

void testMissStatsFormatterAndClear()
{
    InputLatencyTimeline timeline;
    enable(timeline);
    constexpr Nanoseconds budget(16'670'000);
    timeline.beginInput(keyEvent(Qt::Key_Return, 28), ns(0), budget, InputLatencyRefreshSource::Screen, true);
    timeline.endInput(1, ms(1));
    timeline.beforeSynchronizing(ms(2));
    timeline.afterSynchronizing(ms(4));
    timeline.afterRendering(ms(10));
    timeline.frameSwapped(ms(20));
    const InputLatencySample miss = completeSubmitted(timeline, ms(21));
    require(
        miss.late && miss.totalNs == ms(20).count(), "20.00 ms present-queued sample should miss a 16.67 ms budget");
    require(timeline.publish(miss), "miss should publish once");

    const QString line = formatInputLatencyMiss(miss);
    require(line.startsWith(QStringLiteral("input latency:")), "miss line should have stable prefix");
    require(line.contains(QStringLiteral("event=key_press")), "miss line should contain event type");
    require(line.contains(QStringLiteral("key=16777220")), "miss line should contain Qt key code");
    require(line.contains(QStringLiteral("scan=28")), "miss line should contain native scan code");
    require(line.contains(QStringLiteral("budget_ms=16.67")), "miss line should contain budget");
    require(line.contains(QStringLiteral("total_ms=20.00")), "miss line should contain total latency");
    require(
        line.contains(QStringLiteral("present_queue_ms=20.00")), "miss line should contain presentation-queue timing");
    require(line.contains(QStringLiteral("stage=present_queued")), "miss line should contain terminal stage");
    require(line.contains(QStringLiteral("refresh_source=screen")), "miss line should contain refresh source");
    require(line.contains(QStringLiteral("forced_update=0")), "miss line should report that updates are not forced");
    require(!line.contains(QStringLiteral("private typed text")), "miss line must not contain typed text");

    capturedWarnings.clear();
    const QtMessageHandler previousHandler = qInstallMessageHandler(captureWarnings);
    qWarning().noquote() << line;
    qInstallMessageHandler(previousHandler);
    require(capturedWarnings.size() == 1 && capturedWarnings.constFirst() == line,
        "one miss should emit one qWarning-compatible formatted line");
    require(timeline.sampleCount() == 1 && timeline.lateCount() == 1,
        "single published miss should increment sample and late counts once");
    require(timeline.missedFrameCount() == 1, "20 ms response should miss one 16.67 ms frame");
    require(timeline.lastLatencyMs() == 20.0 && timeline.worstLatencyMs() == 20.0,
        "miss should update last and worst latency");
    require(timeline.lastStage() == QStringLiteral("present_queued"), "miss should update last terminal stage");

    timeline.beginInput(keyEvent(Qt::Key_Left, 105), ms(30), budget, InputLatencyRefreshSource::Screen, true);
    timeline.beforeSynchronizing(ms(31));
    timeline.afterRendering(ms(35));
    const InputLatencySample onTime = completeSubmitted(timeline, ms(40));
    require(timeline.publish(onTime), "on-time sample should publish");
    require(timeline.sampleCount() == 2 && timeline.lateCount() == 1, "on-time sample should not increment late count");
    require(timeline.lastLatencyMs() == 10.0 && timeline.worstLatencyMs() == 20.0,
        "statistics should retain worst while updating last latency");

    timeline.beginInput(keyEvent(Qt::Key_Down), ms(50), budget, InputLatencyRefreshSource::Screen, true);
    timeline.beforeSynchronizing(ms(51));
    timeline.afterRendering(ms(60));
    const InputLatencySample multiFrameMiss = completeSubmitted(timeline, ms(101));
    require(timeline.publish(multiFrameMiss), "multi-frame miss should publish");
    require(timeline.missedFrameCount() == 4,
        "51 ms response should add three actually missed frames to the cumulative count");

    timeline.clearStatistics();
    require(timeline.enabled(), "clear should leave enabled state unchanged");
    require(timeline.sampleCount() == 0 && timeline.lateCount() == 0, "clear should reset counters");
    require(timeline.missedFrameCount() == 0, "clear should reset the missed-frame count");
    require(timeline.lastLatencyMs() == 0.0 && timeline.worstLatencyMs() == 0.0 && timeline.lastStage().isEmpty(),
        "clear should reset last, worst, and stage statistics");
    require(!timeline.nextDeadline(), "clear should cancel pending and current measurements");
}

void testPointerMoveWarningPolicy()
{
    InputLatencySample sample;
    sample.event.kind = InputLatencyEventKind::MouseMove;
    sample.budgetNs = ms(8).count();
    sample.totalNs = ms(42).count();
    sample.late = true;
    require(!shouldWarnInputLatency(sample), "video-paced 42 ms mouse movement should not warn");

    sample.totalNs = ms(101).count();
    require(shouldWarnInputLatency(sample), "visible 101 ms mouse movement stall should warn");

    sample.event.kind = InputLatencyEventKind::KeyPress;
    sample.totalNs = ms(42).count();
    require(shouldWarnInputLatency(sample), "non-pointer input should retain the two-frame warning threshold");
}

void testUiLatencyFormatter()
{
    const UiLatencySample sample {
        QStringLiteral("route:settings"),
        QStringLiteral("home"),
        QStringLiteral("settings"),
        QStringLiteral("hit"),
        ms(16).count(),
        ms(300).count(),
        ms(120).count(),
        ms(310).count(),
        ms(20).count(),
        ms(10).count(),
        ms(20).count(),
        ms(5).count(),
        ms(4).count(),
        19,
        3,
        4,
        1,
        { ms(10).count(), ms(20).count(), ms(100).count(), ms(150).count(), ms(260).count(), ms(280).count() },
    };
    const QString line = formatUiLatency(sample);
    require(line.startsWith(QStringLiteral("ui latency: name=route:settings")),
        "UI latency line should identify the measured transition");
    require(line.contains(QStringLiteral("wall_ms=300.00")), "UI latency line should contain total latency");
    require(line.contains(QStringLiteral("gui_cpu_ms=120.00")), "UI latency line should contain GUI CPU time");
    require(line.contains(QStringLiteral("present_ms=20.00")), "UI latency line should isolate presentation");
    require(line.contains(QStringLiteral("budget_intervals=19")), "UI latency line should report budget intervals");
    require(line.contains(QStringLiteral("actual_swaps=3")), "UI latency line should report actual swaps");
    require(line.endsWith(QStringLiteral("stage=content_presented")),
        "UI latency line should identify the content presentation stage");
}

} // namespace

SPOOL_TEST_MAIN("input-latency-monitor")
{
    testDisabledCapture();
    testEventClassifier();
    testCoalescingAndFirstDispatch();
    testStalePreInputFrames();
    testInputDuringRendering();
    testIndependentDeadlines();
    testExactPeriodBoundary();
    testEveryTerminalStage();
    testEpochCancellationAndSemantics();
    testDeadlineCompletionRaces();
    testMissStatsFormatterAndClear();
    testPointerMoveWarningPolicy();
    testUiLatencyFormatter();
    return 0;
}
