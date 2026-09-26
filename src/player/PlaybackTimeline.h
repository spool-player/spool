#pragma once

#include "../media/MediaTypes.h"

#include <QString>

namespace Spool {

class PlaybackTimeline final {
public:
    struct TrickplayFrame {
        bool available = false;
        int sheetIndex = 0;
        int width = 0;
        int height = 0;
        int offsetX = 0;
        int offsetY = 0;
        int sheetWidth = 0;
        int sheetHeight = 0;
    };

    void setSession(const PlaybackSession& session);
    void clear();
    bool updatePosition(double seconds);

    QString activeSegmentType() const;
    double activeSegmentEndSeconds() const;

    bool trickplayAvailable() const;
    int trickplaySheetCount() const;
    TrickplayFrame trickplayFrameAt(double seconds) const;
    int trickplayWidth() const;

private:
    std::vector<MediaSegment> m_segments;
    TrickplayInfo m_trickplay;
    QString m_activeSegmentType;
    double m_activeSegmentEndSeconds = 0.0;
};

} // namespace Spool
