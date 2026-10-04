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
    m_activeSegmentType.clear();
    m_activeSegmentEndSeconds = 0.0;
}

void PlaybackTimeline::clear()
{
    m_segments.clear();
    m_activeSegmentType.clear();
    m_activeSegmentEndSeconds = 0.0;
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
