#pragma once
#include "../media/MediaTypes.h"

#include <QString>

namespace Spool {

class PlaybackFailurePolicy final {
public:
    enum class FileEnd { Stopped, Failed, Completed, Interrupted };

    // mpv reports a clean end of file whenever the demuxer runs out of data,
    // including when a network stream is cut off and cannot be reopened. Only
    // an end that reaches the known duration is the item finishing; anything
    // earlier is an interruption and must neither mark it played nor advance
    // the queue.
    static FileEnd classifyFileEnd(bool failed, int mpvReason, double positionSeconds, double durationSeconds);
    // Watched-on-stop is a device policy, not a natural end or queue advance.
    // The position must come from playback, never a pending seek or resume seed.
    static bool watchedOnStop(bool explicitStop, bool failed, bool loaded, bool hasPlaybackPosition,
        double positionSeconds, double durationSeconds, int thresholdPercent);
    // An interrupted stream is resumed only when it got further than where it
    // started, so a source that keeps ending early cannot restart forever.
    static bool shouldResumeInterrupted(double startSeconds, double positionSeconds);
    static bool isRetryableCodecFailure(const QString& playMethod, bool failedBeforeLoad, int mpvError);
    static bool shouldStartCodecFallback(bool retryableCodecFailure, bool alreadyAttempted, bool syncPlayActive);
    static MovieItem retryItem(const MovieItem& item, qint64 positionTicks);
    static void prepareFallbackSession(PlaybackSession& session, const std::vector<PlaybackQueueItem>& queue,
        int audioStreamIndex, int subtitleStreamIndex);
};

} // namespace Spool
