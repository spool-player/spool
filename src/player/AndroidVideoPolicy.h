#pragma once

#include "media/MediaTypes.h"

#include <functional>

namespace Spool {

// Direct surfaces cannot display a decoder's software fallback. Query only
// facts about the delivered stream; unknown support uses the rendered path.
inline bool androidUsesEmbeddedVideo(
    const PlaybackSession& session, bool directRequested, const std::function<bool(const MediaStreamInfo&)>& supported)
{
    if (!directRequested || session.playMethod == QLatin1String("Transcode"))
        return true;
    for (const auto& stream : session.mediaStreams) {
        if (stream.type.compare(QLatin1String("video"), Qt::CaseInsensitive) != 0)
            continue;
        return stream.codec.isEmpty() || stream.profile.isEmpty() || stream.width <= 0 || stream.height <= 0
            || !supported(stream);
    }
    return true;
}

}
