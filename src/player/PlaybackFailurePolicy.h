#pragma once
#include "../media/MediaTypes.h"

#include <QString>

namespace Spool {

class PlaybackFailurePolicy final {
public:
    static bool isRetryableCodecFailure(const QString& playMethod, bool failedBeforeLoad, int mpvError);
    static bool shouldStartCodecFallback(bool retryableCodecFailure, bool alreadyAttempted, bool syncPlayActive);
    static MovieItem retryItem(const MovieItem& item, qint64 positionTicks);
    static void prepareFallbackSession(PlaybackSession& session, const std::vector<PlaybackQueueItem>& queue,
        int audioStreamIndex, int subtitleStreamIndex);
};

} // namespace Spool
