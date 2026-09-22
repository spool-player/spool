#pragma once

#include "ArtworkSource.h"
#include "Catalog.h"
#include "PlaybackSource.h"
#include "Provider.h"
#include "SearchSource.h"
#include "StreamQualityControl.h"
#include "UserItemStateSink.h"

#include <QHash>
#include <QPointer>
#include <QTimer>

#include <vector>

namespace JellyfinNative {

class ProviderRegistry;

// The one Provider the app sees: every enabled account behind it at once.
//
// IDs are scoped here and nowhere else. Every ID a source hands out leaves
// as "<8 hex of its account>:<the source's own id>", so routes, caches, the
// queue and QML keep treating IDs as opaque strings and a call carrying one
// finds its way back to the account it came from. Lists that have no single
// owner (libraries, home rows, search) are asked of every account in
// parallel and merged; one slow or failing account never empties the rest.
class SourceHub final : public Provider,
                        public Catalog,
                        public SearchSource,
                        public UserItemStateSink,
                        public ArtworkSource,
                        public StreamQualityControl {
    Q_OBJECT

public:
    explicit SourceHub(ProviderRegistry *registry, QObject *parent = nullptr);
    ~SourceHub() override;

    QString id() const override
    {
        return QStringLiteral("hub");
    }
    QString displayName() const override;
    Capabilities capabilities() const override
    {
        return m_capabilities;
    }
    PlaybackSource *playback() override;
    Catalog *catalog() override
    {
        return this;
    }
    ArtworkSource *artwork() override
    {
        return this;
    }
    SearchSource *search() override
    {
        return this;
    }
    UserItemStateSink *itemState() override
    {
        return this;
    }
    StreamQualityControl *streamQuality() override
    {
        return this;
    }
    bool ready() const override;

    // Scoping. `accountOf` answers for any scoped ID, empty when unscoped.
    QString scoped(const QString& accountId, const QString& rawId) const;
    QString accountOf(const QString& scopedId) const;
    static QString rawId(const QString& scopedId);
    Provider *source(const QString& accountId) const;
    std::vector<Provider *> sources() const;
    // Calls an operation on the account behind a scoped or account ID.
    QCoro::Task<QVariantMap> call(QString accountId, QString operation, QVariantMap arguments = {});
    void setVideoCodecs(QStringList codecs, bool restrict);

    // Item menu entries the owning provider declared for this kind of item.
    Q_INVOKABLE QVariantList itemActions(const QString& itemId, const QString& itemType) const;
    // Runs one; the provider may answer with a message, a change, or its own
    // picker (a playlist to add to) before finishing.
    Q_INVOKABLE void runItemAction(const QString& actionId, const QString& itemId, const QString& itemType);
    // The viewer's standing streaming limits from settings.
    void setPlaybackPreferences(qint64 manualMaxBitrate, bool unlimitedLocalNetwork, bool preferRemux, int maxHeight);

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

    // SearchSource: every searchable account at once.
    QCoro::Task<std::vector<MovieItem>> searchItems(QString searchTerm, int limit = 80) override;
    QCoro::Task<std::vector<MovieItem>> fetchSearchSuggestions(int limit = 20) override;

    // UserItemStateSink
    QCoro::Task<void> setItemFavorite(QString itemId, bool favorite) override;
    QCoro::Task<void> setItemPlayed(QString itemId, bool played) override;
    QCoro::Task<void> setItemPlaybackPosition(QString itemId, qint64 positionTicks) override;

    QString imageUrl(const ImageRequest& request) const override;

    // StreamQualityControl
    qint64 bitrateOverride() const override
    {
        return m_bitrate;
    }
    int heightOverride() const override
    {
        return m_height;
    }
    void setOverride(qint64 bitrate, int height) override;
    QString autoDescription() const override
    {
        return QStringLiteral("Original quality");
    }
    std::vector<Rung> ladder(qint64 sourceBitrate, int sourceHeight = 0) const override
    {
        return defaultLadder(sourceBitrate, sourceHeight);
    }

signals:
    void accountEvent(const QString& accountId, const QString& type, const QVariantMap& payload);

private:
    class Playback;
    struct Entry {
        QString accountId;
        QPointer<Provider> provider;
    };

    void addSource(Provider *provider);
    void removeSource(const QString& accountId);
    void refresh();
    void pushPlaybackContext();
    Provider *owner(const QString& scopedId) const;
    MovieItem scopedItem(MovieItem item, const QString& accountId) const;
    std::vector<MovieItem> scopedItems(std::vector<MovieItem> items, const QString& accountId) const;
    template <typename Fetch>
    QCoro::Task<std::vector<MovieItem>> gather(Fetch fetch, int limit, bool searchOnly = false);

    ProviderRegistry *m_registry;
    QHash<QString, Entry> m_entries; // by prefix
    Capabilities m_capabilities;
    Playback *m_playback = nullptr;
    QTimer m_settled;
    bool m_announced = false;
    qint64 m_bitrate = 0;
    int m_height = 0;
    QStringList m_videoCodecs;
    bool m_restrictVideoCodecs = false;
    QVariantMap m_preferences;
    QString m_lastDetailsAccount;
};

} // namespace JellyfinNative
