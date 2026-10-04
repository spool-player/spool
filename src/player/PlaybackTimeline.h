#pragma once

#include "../media/MediaTypes.h"

#include <QString>

namespace Spool {

class PlaybackTimeline final {
public:
    void setSession(const PlaybackSession& session);
    void clear();
    bool updatePosition(double seconds);

    QString activeSegmentType() const;
    double activeSegmentEndSeconds() const;

private:
    std::vector<MediaSegment> m_segments;
    QString m_activeSegmentType;
    double m_activeSegmentEndSeconds = 0.0;
};

} // namespace Spool
