#pragma once

#include <QElapsedTimer>

namespace Spool {

// Where playback is, and what a new seek gesture should be measured from.
//
// mpv's clock cannot be believed across a seek: it keeps reporting the old
// position until the seek completes, and a keyframe seek lands wherever the
// nearest keyframe is. So a seek in flight makes this the authority instead --
// it holds the position the seek was aimed at, which is what the next gesture
// compounds on -- and mpv becomes the authority again the moment the seek
// settles. Nothing here guesses whether a sample is stale; the player says
// when a seek was issued and when mpv reported it finished.
class PlaybackPositionTracker final {
public:
    // Seeded with where playback is about to resume from, so a restart shows
    // the position it is starting at rather than zero until mpv reports in.
    void reset(double startSeconds = 0.0);
    void clear();

    double position() const;
    double estimatedPosition(double playbackSpeed, bool advancing) const;
    double duration() const;
    void setDuration(double seconds);
    double clamp(double seconds) const;

    // A seek has gone to mpv. The reported position moves to the target at
    // once, so a gesture chained onto it compounds rather than starting again
    // from where the picture happens to be.
    void beginSeek(double targetSeconds);
    // A seek whose landing place we do not know -- a chapter step -- so the
    // position stands until mpv reports where it went.
    void beginBlindSeek();
    // Re-aim the seek that has not been handed to mpv yet, rather than
    // counting a second one that will never be dispatched.
    void replaceSeek(double targetSeconds);
    // mpv restarted playback: the oldest seek still in flight has landed.
    void settleSeek();
    // Nothing is coming back for the seeks in flight -- the watchdog fired, the
    // command failed, or the core is going away. The position stands where the
    // last seek aimed it and mpv is believed again from its next report.
    void abandonSeeks();
    bool seekInFlight() const;

    // Returns whether the reported position moved enough to be worth
    // repainting. Samples that arrive while a seek is in flight are dropped.
    bool update(double seconds);

private:
    double m_positionSeconds = 0.0;
    double m_durationSeconds = 0.0;
    quint64 m_issuedSeeks = 0;
    quint64 m_settledSeeks = 0;
    bool m_hasMpvPosition = false;
    QElapsedTimer m_positionClock;
};

} // namespace Spool
