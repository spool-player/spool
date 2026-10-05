#include "player/PlaybackTimeline.h"

#include "TestMain.h"

#include <QCoreApplication>
#include <QDebug>

#include <cmath>

using Spool::MediaSegment;
using Spool::PlaybackSession;
using Spool::PlaybackTimeline;

namespace {

int failures = 0;

void expect(bool condition, const char *message)
{
    if (condition)
        return;
    qCritical() << message;
    ++failures;
}

void testSegments()
{
    PlaybackSession session;
    session.segments = {
        MediaSegment { QStringLiteral("intro"), QStringLiteral("Intro"), 10 * 10000000LL, 30 * 10000000LL },
        MediaSegment { QStringLiteral("outro"), QStringLiteral("Outro"), 90 * 10000000LL, 100 * 10000000LL },
    };

    PlaybackTimeline timeline;
    timeline.setSession(session);
    expect(!timeline.updatePosition(5.0), "position before a segment remains inactive");
    expect(timeline.updatePosition(10.0), "entering a segment reports a change");
    expect(timeline.activeSegmentType() == QStringLiteral("Intro"), "intro segment becomes active");
    expect(std::abs(timeline.activeSegmentEndSeconds() - 30.0) < 0.001, "active segment exposes its end");
    expect(!timeline.updatePosition(20.0), "remaining in a segment reports no change");
    expect(timeline.updatePosition(29.5), "segment hides during its final half second");
    expect(timeline.activeSegmentType().isEmpty(), "segment clears near its end");
}

void testStreamOrigin()
{
    PlaybackSession session;
    session.timelineOriginTicks = 437'000'000;
    session.runtimeTicks = 100 * 10'000'000LL;
    PlaybackTimeline timeline;
    timeline.setSession(session);
    expect(std::abs(timeline.streamSeconds(43.7)) < 0.001, "server-resumed media needs no second resume seek");
    expect(std::abs(timeline.sourceSeconds(2.0) - 45.7) < 0.001, "media samples report absolute source positions");
    expect(std::abs(timeline.streamSeconds(60.0) - 16.3) < 0.001, "absolute seeks subtract the server origin");
    expect(timeline.sourceDuration(56.3) == 100.0, "a resumed stream retains the full source runtime");
    expect(!timeline.containsPosition(30.0) && timeline.containsPosition(43.7),
        "a backwards seek outside a clipped stream must resolve again");
    timeline.clear();
    expect(timeline.sourceSeconds(2.0) == 2.0 && timeline.streamSeconds(60.0) == 60.0
            && timeline.sourceDuration(100.0) == 100.0 && timeline.containsPosition(0.0),
        "direct/full-timeline sessions and reset preserve ordinary media coordinates");
}

} // namespace

SPOOL_TEST_MAIN("playback-timeline")
{
    QCoreApplication application(argc, argv);
    testSegments();
    testStreamOrigin();
    return failures == 0 ? 0 : 1;
}
