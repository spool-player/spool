#include "PlaybackTimeline.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Spool {

namespace {

    constexpr double kTicksPerSecond = 10000000.0;

} // namespace

void PlaybackTimeline::setSession(const PlaybackSession& session)
{
    m_segments = session.segments;
    m_trickplay = session.trickplay;
    m_activeSegmentType.clear();
    m_activeSegmentEndSeconds = 0.0;
}

void PlaybackTimeline::clear()
{
    m_segments.clear();
    m_trickplay = {};
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

bool PlaybackTimeline::trickplayAvailable() const
{
    return m_trickplay.intervalMs > 0 && m_trickplay.tileWidth > 0 && m_trickplay.tileHeight > 0
        && m_trickplay.width > 0 && m_trickplay.height > 0;
}

int PlaybackTimeline::trickplaySheetCount() const
{
    if (!trickplayAvailable())
        return 0;

    const int tileCount = m_trickplay.tileWidth * m_trickplay.tileHeight;
    if (tileCount <= 0)
        return 0;

    const int thumbnailCount = m_trickplay.thumbnailCount > 0 ? m_trickplay.thumbnailCount : 1;
    return std::max(1, (thumbnailCount + tileCount - 1) / tileCount);
}

PlaybackTimeline::TrickplayFrame PlaybackTimeline::trickplayFrameAt(double seconds) const
{
    TrickplayFrame frame;
    if (!trickplayAvailable() || !std::isfinite(seconds))
        return frame;

    const double tileValue = std::floor(std::max(0.0, seconds) * 1000.0 / m_trickplay.intervalMs);
    if (tileValue > std::numeric_limits<int>::max())
        return frame;

    const int currentTile = static_cast<int>(tileValue);
    if (m_trickplay.thumbnailCount > 0 && currentTile >= m_trickplay.thumbnailCount) {
        return frame;
    }

    const int tileCount = m_trickplay.tileWidth * m_trickplay.tileHeight;
    if (tileCount <= 0)
        return frame;

    const int tileOffset = currentTile % tileCount;
    frame.available = true;
    frame.sheetIndex = currentTile / tileCount;
    frame.width = m_trickplay.width;
    frame.height = m_trickplay.height;
    frame.offsetX = -(tileOffset % m_trickplay.tileWidth) * m_trickplay.width;
    frame.offsetY = -(tileOffset / m_trickplay.tileWidth) * m_trickplay.height;
    frame.sheetWidth = m_trickplay.width * m_trickplay.tileWidth;
    frame.sheetHeight = m_trickplay.height * m_trickplay.tileHeight;
    return frame;
}

int PlaybackTimeline::trickplayWidth() const
{
    return m_trickplay.width;
}

} // namespace Spool
