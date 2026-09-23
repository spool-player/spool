#include "player/PlaybackPositionTracker.h"

#include "TestMain.h"

#include <QDebug>

#include <cmath>
#include <cstdlib>

using Spool::PlaybackPositionTracker;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    qCritical() << message;
    std::exit(EXIT_FAILURE);
}

bool near(double actual, double expected)
{
    return std::abs(actual - expected) < 0.01;
}

} // namespace

SPOOL_TEST_MAIN("playback-position-tracker")
{
    PlaybackPositionTracker tracker;
    tracker.reset();
    tracker.setDuration(100.0);
    require(near(tracker.position(), 0.0), "reset invented a position before mpv reported one");
    require(near(tracker.clamp(120.0), 100.0), "duration clamp failed");

    tracker.update(40.0);
    require(near(tracker.position(), 40.0), "an mpv sample outside a seek was not taken");
    tracker.update(41.0);
    require(near(tracker.position(), 41.0), "mpv is the position when no seek is in flight");

    // A seek moves the position at once and holds it there: mpv keeps
    // reporting where playback was until the seek completes.
    tracker.beginSeek(80.0);
    require(tracker.seekInFlight(), "a dispatched seek was not counted as in flight");
    require(near(tracker.position(), 80.0), "a seek did not move the position to its target");
    tracker.update(41.5);
    require(near(tracker.position(), 80.0), "a sample from before the seek was taken");
    require(near(tracker.estimatedPosition(1.0, true), 80.0), "the seek target was extrapolated past");

    // The restart is what says the seek landed; from there mpv is the
    // position again, keyframe rounding and all.
    tracker.settleSeek();
    require(!tracker.seekInFlight(), "a settled seek stayed in flight");
    tracker.update(78.5);
    require(near(tracker.position(), 78.5), "mpv's own position after a seek was not taken");

    // The bug this is built around: a second gesture measures from where the
    // first one was aimed, not from where playback still is, and a third from
    // the second -- never from an older seek's starting point.
    tracker.reset();
    tracker.setDuration(1000.0);
    tracker.update(500.0);
    tracker.beginSeek(510.0);
    tracker.update(500.2);
    tracker.beginSeek(tracker.position() + 10.0);
    require(near(tracker.position(), 520.0), "a chained seek did not compound on the one in flight");
    tracker.beginSeek(tracker.position() + 10.0);
    require(near(tracker.position(), 530.0), "a third seek fell back to an older seek's position");
    tracker.settleSeek();
    require(tracker.seekInFlight(), "one restart settled every seek in flight");
    tracker.update(505.0);
    require(near(tracker.position(), 530.0), "a sample belonging to an earlier seek was taken");
    tracker.settleSeek();
    tracker.settleSeek();
    require(!tracker.seekInFlight(), "seeks did not settle one restart at a time");
    tracker.update(528.0);
    require(near(tracker.position(), 528.0), "the position did not return to mpv once the seeks settled");

    // A seek that never gets confirmed -- mpv folded two into one restart --
    // gives the position back to mpv without giving up where it was aimed.
    tracker.beginSeek(600.0);
    tracker.abandonSeeks();
    require(!tracker.seekInFlight(), "abandoning left a seek in flight");
    require(near(tracker.position(), 600.0), "abandoning a seek dropped back to a stale position");
    tracker.update(598.0);
    require(near(tracker.position(), 598.0), "mpv was not believed again after a seek was abandoned");

    // A seek queued before the core can take one is re-aimed, not counted
    // again: only one command goes out, so only one restart comes back.
    tracker.reset();
    tracker.setDuration(1000.0);
    tracker.beginSeek(100.0);
    tracker.replaceSeek(200.0);
    require(near(tracker.position(), 200.0), "re-aiming a queued seek did not move the position");
    tracker.settleSeek();
    require(!tracker.seekInFlight(), "a re-aimed seek was counted twice");

    // A chapter step lands wherever mpv decides, so the position stands until
    // it reports one -- but the samples on the way there are still stale.
    tracker.reset();
    tracker.setDuration(1000.0);
    tracker.update(300.0);
    tracker.beginBlindSeek();
    require(near(tracker.position(), 300.0), "a blind seek invented a position");
    tracker.update(301.0);
    require(near(tracker.position(), 300.0), "a sample from before a blind seek was taken");
    tracker.settleSeek();
    tracker.update(120.0);
    require(near(tracker.position(), 120.0), "a chapter step backwards was rejected");

    tracker.clear();
    require(near(tracker.position(), 0.0), "clear did not reset position");
    require(near(tracker.duration(), 0.0), "clear did not reset duration");
    require(!tracker.seekInFlight(), "clear left a seek in flight");

    // A restart resumes where the viewer was, so the seeded position stands in
    // until mpv reports one of its own.
    tracker.reset(344.5);
    require(near(tracker.position(), 344.5), "reset did not seed the resume position");
    tracker.setDuration(600.0);
    require(near(tracker.position(), 344.5), "a later duration moved the seeded position");
    tracker.update(345.0);
    require(near(tracker.position(), 345.0), "the first mpv sample after a seeded reset was rejected");
    tracker.reset(-5.0);
    require(near(tracker.position(), 0.0), "a negative seed was not clamped away");
    return EXIT_SUCCESS;
}
