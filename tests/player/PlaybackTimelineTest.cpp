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

void testTrickplay()
{
    PlaybackSession session;
    session.trickplay.width = 320;
    session.trickplay.height = 180;
    session.trickplay.tileWidth = 4;
    session.trickplay.tileHeight = 3;
    session.trickplay.thumbnailCount = 25;
    session.trickplay.intervalMs = 10000;

    PlaybackTimeline timeline;
    timeline.setSession(session);
    expect(timeline.trickplayAvailable(), "valid trickplay metadata is available");
    expect(timeline.trickplaySheetCount() == 3, "sheet count rounds up");

    const auto first = timeline.trickplayFrameAt(-1.0);
    expect(first.available && first.sheetIndex == 0 && first.offsetX == 0 && first.offsetY == 0,
        "negative positions clamp to the first frame");

    const auto secondSheet = timeline.trickplayFrameAt(130.0);
    expect(secondSheet.available && secondSheet.sheetIndex == 1, "frame selects the correct sheet");
    expect(secondSheet.offsetX == -320 && secondSheet.offsetY == 0, "frame exposes sprite offsets");
    expect(secondSheet.sheetWidth == 1280 && secondSheet.sheetHeight == 540, "frame exposes sheet dimensions");

    expect(!timeline.trickplayFrameAt(250.0).available, "positions beyond the manifest are unavailable");
}

} // namespace

SPOOL_TEST_MAIN("playback-timeline")
{
    QCoreApplication application(argc, argv);
    testSegments();
    testTrickplay();
    return failures == 0 ? 0 : 1;
}
