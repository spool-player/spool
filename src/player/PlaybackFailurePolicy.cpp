#include "PlaybackFailurePolicy.h"

#include <algorithm>
#include <mpv/client.h>

namespace Spool {

bool PlaybackFailurePolicy::isRetryableCodecFailure(const QString& playMethod, bool failedBeforeLoad, int mpvError)
{
    if (!failedBeforeLoad
        || (playMethod != QStringLiteral("DirectPlay") && playMethod != QStringLiteral("DirectStream"))) {
        return false;
    }

    // MPV_ERROR_LOADING_FAILED is deliberately excluded: HTTP, authentication,
    // TLS, and many other transport failures collapse to that code. Only the
    // unambiguous demux/container capability errors may trigger renegotiation.
    return mpvError == MPV_ERROR_UNKNOWN_FORMAT || mpvError == MPV_ERROR_UNSUPPORTED;
}
bool PlaybackFailurePolicy::shouldStartCodecFallback(
    bool retryableCodecFailure, bool alreadyAttempted, bool syncPlayActive)
{
    return retryableCodecFailure && !alreadyAttempted && !syncPlayActive;
}

MovieItem PlaybackFailurePolicy::retryItem(const MovieItem& item, qint64 positionTicks)
{
    MovieItem retry = item;
    retry.resumeTicks = std::max<qint64>(0, positionTicks);
    return retry;
}

void PlaybackFailurePolicy::prepareFallbackSession(PlaybackSession& session,
    const std::vector<PlaybackQueueItem>& queue, int audioStreamIndex, int subtitleStreamIndex)
{
    session.nowPlayingQueue = queue;
    session.audioStreamIndex = audioStreamIndex;
    session.subtitleStreamIndex = subtitleStreamIndex;
    session.codecFallback = true;
    session.restoreStreamSelection = true;
}

} // namespace Spool
