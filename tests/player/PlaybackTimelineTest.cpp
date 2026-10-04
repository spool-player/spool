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

} // namespace

SPOOL_TEST_MAIN("playback-timeline")
{
    QCoreApplication application(argc, argv);
    testSegments();
    return failures == 0 ? 0 : 1;
}
