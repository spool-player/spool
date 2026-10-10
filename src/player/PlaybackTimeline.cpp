#include "PlaybackTimeline.h"

#include <algorithm>
#include <cmath>

namespace Spool {

namespace {

    constexpr double kTicksPerSecond = 10000000.0;

} // namespace

void PlaybackTimeline::setSession(const PlaybackSession& session)
{
    m_segments = session.segments;
    m_originSeconds = static_cast<double>(session.timelineOriginTicks) / kTicksPerSecond;
    m_sourceDuration = static_cast<double>(session.runtimeTicks) / kTicksPerSecond;
    m_activeSegmentType.clear();
    m_activeSegmentEndSeconds = 0.0;
}

void PlaybackTimeline::clear()
{
    m_segments.clear();
    m_originSeconds = 0.0;
    m_sourceDuration = 0.0;
    m_activeSegmentType.clear();
    m_activeSegmentEndSeconds = 0.0;
}

double PlaybackTimeline::sourceSeconds(double streamSeconds) const
{
    return streamSeconds + m_originSeconds;
}

double PlaybackTimeline::streamSeconds(double sourceSeconds) const
{
    return std::max(0.0, sourceSeconds - m_originSeconds);
}

double PlaybackTimeline::sourceDuration(double streamDuration) const
{
    return m_originSeconds > 0.0 && m_sourceDuration > 0.0 ? m_sourceDuration : sourceSeconds(streamDuration);
}

bool PlaybackTimeline::containsPosition(double sourceSeconds) const
{
    return sourceSeconds >= m_originSeconds;
}

bool PlaybackTimeline::updatePosition(double seconds)
{
    QString segmentType;
    double segmentEndSeconds = 0.0;

    if (std::isfinite(seconds)) {
        for (const MediaSegment& segment : m_segments) {
            const double start = segment.startTicks / kTicksPerSecond;
            const double end = segment.endTicks / kTicksPerSecond;
            if (seconds >= start && seconds < end - 0.5) {
                segmentType = segment.type;
                segmentEndSeconds = end;
                break;
            }
        }
    }

    if (segmentType == m_activeSegmentType && segmentEndSeconds == m_activeSegmentEndSeconds) {
        return false;
    }

    m_activeSegmentType = segmentType;
    m_activeSegmentEndSeconds = segmentEndSeconds;
    return true;
}

QString PlaybackTimeline::activeSegmentType() const
{
    return m_activeSegmentType;
}

double PlaybackTimeline::activeSegmentEndSeconds() const
{
    return m_activeSegmentEndSeconds;
}

} // namespace Spool
