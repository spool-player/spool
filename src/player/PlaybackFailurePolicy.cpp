#include "PlaybackFailurePolicy.h"

#include <algorithm>
#include <cmath>
#include <mpv/client.h>

namespace Spool {

namespace {
    // Container and playlist durations can disagree with the last decoded
    // frame by a second or two; a real cut-off lands well before this.
    constexpr double kEndToleranceSeconds = 2.0;
    constexpr double kResumeProgressSeconds = 30.0;
} // namespace

PlaybackFailurePolicy::FileEnd PlaybackFailurePolicy::classifyFileEnd(
    bool failed, int mpvReason, double positionSeconds, double durationSeconds)
{
    if (failed)
        return FileEnd::Failed;
    if (mpvReason != MPV_END_FILE_REASON_EOF)
        return FileEnd::Stopped;
    // Without a duration there is nothing to measure against, as for a live
    // stream, so its end is taken at its word.
    if (durationSeconds <= 0.0 || positionSeconds >= durationSeconds - kEndToleranceSeconds)
        return FileEnd::Completed;
    return FileEnd::Interrupted;
}

bool PlaybackFailurePolicy::watchedOnStop(bool explicitStop, bool failed, bool loaded, bool hasPlaybackPosition,
    double positionSeconds, double durationSeconds, int thresholdPercent)
{
    if (!explicitStop || failed || !loaded || !hasPlaybackPosition || !std::isfinite(positionSeconds)
        || !std::isfinite(durationSeconds) || durationSeconds <= 0.0 || positionSeconds < 0.0) {
        return false;
    }
    const int percentage = std::clamp(thresholdPercent, 50, 100);
    return positionSeconds >= durationSeconds * (static_cast<double>(percentage) / 100.0);
}

bool PlaybackFailurePolicy::shouldResumeInterrupted(double startSeconds, double positionSeconds)
{
    return positionSeconds >= startSeconds + kResumeProgressSeconds;
}

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
