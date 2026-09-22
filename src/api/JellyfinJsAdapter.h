#pragma once

#include "../media/MediaTypes.h"
#include "../provider/ArtworkSource.h"
#include "../provider/Catalog.h"
#include "../provider/PlaybackSource.h"
#include "../provider/SearchSource.h"
#include "../provider/StreamQualityControl.h"
#include "../provider/UserItemStateSink.h"

#include <QCoroTask>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantMap>

#include <vector>

namespace JellyfinNative {

class ProviderRegistry;

// Bridges the portable JavaScript Jellyfin provider (spool.jellyfin) into the
// native core interfaces: Catalog, SearchSource, PlaybackSource, ArtworkSource,
// and UserItemStateSink.
//
// Bulk listing and search results are decoded on the worker thread into
// ProviderMediaPage without converting QVariant trees or JSON on the GUI thread.
class JellyfinJsAdapter final : public QObject,
                                public Catalog,
                                public SearchSource,
                                public UserItemStateSink,
                                public ArtworkSource,
                                public StreamQualityControl {
    Q_OBJECT

public:
    explicit JellyfinJsAdapter(ProviderRegistry *registry, QObject *parent = nullptr);
    ~JellyfinJsAdapter() override;

    void configure(const QString& sourceId, const QString& serverUrl, const QString& sessionToken);
    void clear();
    void setDeviceId(const QString& deviceId);

    Catalog *catalog()
    {
        return this;
    }
    SearchSource *search()
    {
        return this;
    }
    UserItemStateSink *itemState()
    {
        return this;
    }
    ArtworkSource *artwork()
    {
        return this;
    }
    PlaybackSource *playback();

    // Catalog
    bool signedIn() const override;
    QString libraryScopeKey() const override;
    QCoro::Task<PagedMovieItems> fetchBrowsePage(
        BrowseDescriptor descriptor, int startIndex = 0, int limit = 72, QVariantMap queryOptions = {}) override;
    QCoro::Task<MovieItem> fetchItemDetails(QString itemId) override;
    QCoro::Task<std::vector<MovieItem>> fetchSeasons(QString seriesId) override;
    QCoro::Task<std::vector<MovieItem>> fetchEpisodes(QString seriesId, QString seasonId = {}) override;
    QCoro::Task<std::vector<MovieItem>> fetchResumeItems(int limit = 24) override;
    QCoro::Task<std::vector<MovieItem>> fetchNextUpEpisodes(int limit = 24) override;
    QCoro::Task<std::vector<MovieItem>> fetchLatestItems(QString parentId = {}, int limit = 24) override;
    QCoro::Task<std::vector<MovieItem>> fetchSimilarItems(QString itemId, int limit = 24) override;
    QCoro::Task<PersonCredits> fetchItemsByPerson(QString personId, int maximumItems = 4000) override;
    QCoro::Task<std::vector<LibraryItem>> fetchLibraries() override;
    QCoro::Task<QVariantMap> fetchLibraryFilterOptions(QString libraryId, QString collectionType = {}) override;
    QCoro::Task<std::vector<MovieItem>> fetchItemsByIds(QStringList itemIds) override;

    // SearchSource
    QCoro::Task<std::vector<MovieItem>> searchItems(QString searchTerm, int limit = 80) override;
    QCoro::Task<std::vector<MovieItem>> fetchSearchSuggestions(int limit = 20) override;

    // UserItemStateSink
    QCoro::Task<void> setItemFavorite(QString itemId, bool favorite) override;
    QCoro::Task<void> setItemPlayed(QString itemId, bool played) override;
    QCoro::Task<void> setItemPlaybackPosition(QString itemId, qint64 positionTicks) override;

    // ArtworkSource
    QString imageUrl(const ImageRequest& request) const override;

    StreamQualityControl *streamQuality()
    {
        return this;
    }

    // StreamQualityControl
    qint64 bitrateOverride() const override
    {
        return m_bitrateOverride;
    }
    int heightOverride() const override
    {
        return m_heightOverride;
    }
    void setOverride(qint64 bitrate, int height) override
    {
        m_bitrateOverride = bitrate;
        m_heightOverride = height;
    }
    QString autoDescription() const override
    {
        return QStringLiteral("Direct Play");
    }
    std::vector<Rung> ladder(qint64 sourceBitrate) const override
    {
        return StreamQualityControl::defaultLadder(sourceBitrate);
    }
    void setVideoCodecCapabilities(QStringList videoCodecs, bool restrictVideoCodecs);

    // Playback
    QCoro::Task<PlaybackSession> resolvePlayback(MovieItem item, bool forceTranscode);
    QCoro::Task<std::vector<MediaSegment>> fetchMediaSegments(QString itemId);
    QByteArray mediaRequestHeaders() const;
    QUrl mediaOrigin() const;
    QString trickplayTileUrl(const QString& itemId, int width, int tileIndex) const;
    QCoro::Task<void> reportPlaybackStart(PlaybackSession session, double playbackRate, int volume, bool muted);
    QCoro::Task<void> reportPlaybackProgress(
        PlaybackSession session, qint64 positionTicks, bool paused, double playbackRate, int volume, bool muted);
    QCoro::Task<void> reportPlaybackStopped(
        PlaybackSession session, qint64 positionTicks, bool failed, double playbackRate);

private:
    class Playback;

    ProviderRegistry *m_registry = nullptr;
    Playback *m_playback = nullptr;
    QString m_sourceId;
    QString m_serverUrl;
    QString m_sessionToken;
    QString m_deviceId;
    qint64 m_bitrateOverride = 0;
    int m_heightOverride = 0;
    QStringList m_videoCodecs;
    bool m_restrictVideoCodecs = false;
};

} // namespace JellyfinNative
