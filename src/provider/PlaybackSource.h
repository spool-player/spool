#pragma once

#include "../media/MediaTypes.h"

#include <QCoroTask>

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

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
    // How many parallel range requests the network profile allows per stream.
    virtual int playbackParallelRequests() const = 0;
    // Whether reports and follow-up lookups can be made right now.
    virtual bool signedIn() const = 0;
    virtual QString trickplayTileUrl(const QString& itemId, int width, int tileIndex) const = 0;

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
