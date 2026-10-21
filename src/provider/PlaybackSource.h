#pragma once

#include "../media/MediaTypes.h"

#include <QCoroTask>

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantMap>

#include <vector>

namespace Spool {

// What the player needs from wherever media comes from. This is exactly the
// surface src/player used from the Jellyfin facade and nothing more; a
// provider that keeps no playback state server-side implements the report
// calls as completed no-ops.
class PlaybackSource : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // Header lines mpv sends with every media request, one per line in the
    // "Name: value" form libmpv's http-header-fields takes. Empty when the
    // source needs none.
    virtual QByteArray mediaRequestHeaders() const = 0;
    // The origin whose remembered TLS trust decision covers media URLs.
    virtual QUrl mediaOrigin() const = 0;
    // Hard range-request cap; the learned initial count lives in the profile.
    virtual int playbackParallelRequests() const = 0;
    // Native-only transport learning. Rates are bytes/s; media bitrate is bits/s.
    // {initial-requests:1..4, latency-ms, capacity-bytes-per-second,
    //  measured-bitrate:bits/s, concurrency-samples:[{requests, capacity-bytes-per-second,
    //  latency-ms, updated-at-ms}]} contains only fresh observations.
    // Feedback is curl-transport-metrics plus observed-at-ms (UTC epoch ms)
    // and media-bitrate (bits/s). Only non-cache-blocked concurrency samples
    // with increasing stream-id/capacity-samples are learned.
    virtual QVariantMap playbackNetworkProfile() const
    {
        return {};
    }
    // origin is captured with the load, never inferred from a later account.
    virtual void updatePlaybackNetworkProfile(const QUrl&, const QVariantMap&) { }
    // Whether reports and follow-up lookups can be made right now.
    virtual bool signedIn() const = 0;
    // Reporting retries must retain the source instance that accepted playback,
    // rather than route an old session through a replacement account runtime.
    virtual PlaybackSource *reportingContext(const QString&)
    {
        return this;
    }

    // Turns an item into something mpv can open: the URL, its streams, and
    // where to start. `forceTranscode` asks a source that can re-encode to
    // do so after a direct stream failed to decode; a source that cannot
    // ignores it.
    virtual QCoro::Task<PlaybackSession> resolvePlayback(MovieItem item, bool forceTranscode) = 0;
    // Intro and credit markers; empty for a source without them.
    virtual QCoro::Task<std::vector<MediaSegment>> fetchMediaSegments(QString itemId) = 0;

    // Every episode of a series in play order, so the queue can continue
    // past the one the viewer started.
    virtual QCoro::Task<std::vector<MovieItem>> fetchSeriesEpisodes(QString seriesId) = 0;

    virtual QCoro::Task<void> reportPlaybackStart(PlaybackSession session, double playbackRate, int volume, bool muted)
        = 0;
    virtual QCoro::Task<void> reportPlaybackProgress(
        PlaybackSession session, qint64 positionTicks, bool paused, double playbackRate, int volume, bool muted)
        = 0;
    virtual QCoro::Task<void> reportPlaybackStopped(
        PlaybackSession session, qint64 positionTicks, bool failed, double playbackRate)
        = 0;

signals:
    // The media request headers are no longer valid; the player drops the
    // ones it handed to mpv.
    void credentialsChanged();
    // The parallel request budget changed; a prepared mpv must be rebuilt.
    void playbackNetworkProfileChanged();
};

} // namespace Spool
