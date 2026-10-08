#include "player/PlaybackFailurePolicy.h"

#include "TestMain.h"
#include "TestRequire.h"

#include <mpv/client.h>

#include <cstdlib>
#include <iostream>

namespace {

using SpoolTests::require;

} // namespace

SPOOL_TEST_MAIN("playback-failure-policy")
{
    using Spool::PlaybackFailurePolicy;

    require(
        PlaybackFailurePolicy::isRetryableCodecFailure(QStringLiteral("DirectPlay"), true, MPV_ERROR_UNKNOWN_FORMAT),
        "an early direct-play container failure should retry through the server");
    require(PlaybackFailurePolicy::isRetryableCodecFailure(QStringLiteral("DirectStream"), true, MPV_ERROR_UNSUPPORTED),
        "an early direct-stream codec failure should retry through the server");
    require(
        !PlaybackFailurePolicy::isRetryableCodecFailure(QStringLiteral("DirectPlay"), true, MPV_ERROR_LOADING_FAILED),
        "network, authentication, and TLS-shaped load failures must not masquerade as codec errors");
    require(
        !PlaybackFailurePolicy::isRetryableCodecFailure(QStringLiteral("DirectPlay"), false, MPV_ERROR_UNKNOWN_FORMAT),
        "failure after playback has loaded must not restart a watched item");
    require(!PlaybackFailurePolicy::isRetryableCodecFailure(QStringLiteral("Transcode"), true, MPV_ERROR_UNSUPPORTED),
        "a failed fallback session must not recurse");

    require(PlaybackFailurePolicy::shouldStartCodecFallback(true, false, false),
        "the first classified direct-play failure should start one fallback");
    require(!PlaybackFailurePolicy::shouldStartCodecFallback(true, true, false),
        "a fallback failure must be surfaced instead of retried");
    require(!PlaybackFailurePolicy::shouldStartCodecFallback(true, false, true),
        "a SyncPlay member must not independently schedule a replacement stream");

    using FileEnd = PlaybackFailurePolicy::FileEnd;
    require(
        PlaybackFailurePolicy::classifyFileEnd(false, MPV_END_FILE_REASON_EOF, 1200.0, 2640.0) == FileEnd::Interrupted,
        "a stream cut off mid-episode must not count as finishing it");
    require(
        PlaybackFailurePolicy::classifyFileEnd(false, MPV_END_FILE_REASON_EOF, 2638.5, 2640.0) == FileEnd::Completed,
        "an end within the duration's rounding is the item finishing");
    require(PlaybackFailurePolicy::classifyFileEnd(false, MPV_END_FILE_REASON_EOF, 95.0, 0.0) == FileEnd::Completed,
        "a stream with no known duration ends when it says it does");
    require(PlaybackFailurePolicy::classifyFileEnd(false, MPV_END_FILE_REASON_STOP, 2639.0, 2640.0) == FileEnd::Stopped,
        "a stop near the end is still a stop");
    require(PlaybackFailurePolicy::classifyFileEnd(true, MPV_END_FILE_REASON_ERROR, 1200.0, 2640.0) == FileEnd::Failed,
        "an mpv error stays a failure");

    require(PlaybackFailurePolicy::shouldResumeInterrupted(0.0, 1200.0),
        "an interruption after real progress resumes where it broke");
    require(!PlaybackFailurePolicy::shouldResumeInterrupted(1200.0, 1200.0),
        "a resume that ends where it started must not resume again");

    Spool::MovieItem original;
    original.id = QStringLiteral("item");
    original.resumeTicks = 1;
    const Spool::MovieItem retry = PlaybackFailurePolicy::retryItem(original, 42'000'000);
    require(retry.id == original.id && retry.resumeTicks == 42'000'000,
        "fallback should preserve the item while resuming at the failed position");

    Spool::PlaybackSession fallback;
    fallback.playMethod = QStringLiteral("Transcode");
    const std::vector<Spool::PlaybackQueueItem> queue {
        { QStringLiteral("item"), QStringLiteral("playlist-item") },
        { QStringLiteral("next"), QStringLiteral("playlist-next") },
    };
    PlaybackFailurePolicy::prepareFallbackSession(fallback, queue, 3, 7);
    require(fallback.codecFallback && fallback.restoreStreamSelection && fallback.nowPlayingQueue.size() == 2
            && fallback.nowPlayingQueue[0].playlistItemId == QStringLiteral("playlist-item")
            && fallback.nowPlayingQueue[1].itemId == QStringLiteral("next") && fallback.audioStreamIndex == 3
            && fallback.subtitleStreamIndex == 7,
        "fallback should preserve queue and selected stream state");

    return EXIT_SUCCESS;
}
