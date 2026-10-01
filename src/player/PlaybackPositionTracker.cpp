#include "PlaybackPositionTracker.h"

#include <QtGlobal>

#include <cmath>

namespace Spool {

void PlaybackPositionTracker::reset(double startSeconds)
{
    const double start = std::isfinite(startSeconds) ? qMax(0.0, startSeconds) : 0.0;
    m_positionSeconds = start;
    m_durationSeconds = 0.0;
    m_issuedSeeks = 0;
    m_settledSeeks = 0;
    m_hasMpvPosition = false;
    m_positionClock.invalidate();
}

void PlaybackPositionTracker::clear()
{
    reset(0.0);
}

double PlaybackPositionTracker::position() const
{
    return m_positionSeconds;
}

double PlaybackPositionTracker::estimatedPosition(double playbackSpeed, bool advancing) const
{
    // A seek in flight is holding the position at its target; running the
    // clock forward from there would drift the seek bar away from the place
    // the next gesture is going to be measured from.
    if (!advancing || seekInFlight() || !m_hasMpvPosition || !m_positionClock.isValid()
        || !std::isfinite(playbackSpeed)) {
        return m_positionSeconds;
    }
    return clamp(m_positionSeconds + static_cast<double>(m_positionClock.elapsed()) * playbackSpeed / 1'000.0);
}

double PlaybackPositionTracker::duration() const
{
    return m_durationSeconds;
}

void PlaybackPositionTracker::setDuration(double seconds)
{
    m_durationSeconds = std::isfinite(seconds) ? qMax(0.0, seconds) : 0.0;
    m_positionSeconds = clamp(m_positionSeconds);
}

double PlaybackPositionTracker::clamp(double seconds) const
{
    if (!std::isfinite(seconds))
        return m_positionSeconds;
    if (m_durationSeconds > 0.0)
        return qBound(0.0, seconds, m_durationSeconds);
    return qMax(0.0, seconds);
}

void PlaybackPositionTracker::beginSeek(double targetSeconds)
{
    ++m_issuedSeeks;
    m_positionSeconds = clamp(targetSeconds);
    m_positionClock.invalidate();
}

void PlaybackPositionTracker::beginBlindSeek()
{
    ++m_issuedSeeks;
    m_positionClock.invalidate();
}

void PlaybackPositionTracker::replaceSeek(double targetSeconds)
{
    if (!seekInFlight()) {
        beginSeek(targetSeconds);
        return;
    }
    m_positionSeconds = clamp(targetSeconds);
    m_positionClock.invalidate();
}

void PlaybackPositionTracker::settleSeek()
{
    if (seekInFlight())
        ++m_settledSeeks;
}

void PlaybackPositionTracker::abandonSeeks()
{
    m_settledSeeks = m_issuedSeeks;
}

bool PlaybackPositionTracker::seekInFlight() const
{
    return m_settledSeeks < m_issuedSeeks;
}

bool PlaybackPositionTracker::update(double seconds)
{
    if (!std::isfinite(seconds) || seekInFlight())
        return false;

    const double clamped = clamp(seconds);
    m_hasMpvPosition = true;
    m_positionClock.restart();
    const bool changed = std::abs(m_positionSeconds - clamped) >= 0.05;
    m_positionSeconds = clamped;
    return changed;
}

} // namespace Spool
