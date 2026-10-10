#pragma once

#include "ArtworkSource.h"
#include "Catalog.h"
#include "DownloadSource.h"
#include "PlaybackSource.h"
#include "Provider.h"
#include "SearchSource.h"
#include "UserItemStateSink.h"

#include "ProviderMediaPage.h"
#include <QFuture>

namespace Spool {

class ProviderRegistry;

// One account on a JavaScript provider, seen as native interfaces. Listing
// and details are decoded on the provider's worker; artwork templates and
// per-playback preview descriptors draw without calling back into JS.
class PortableProvider final : public Provider,
                               public Catalog,
                               public SearchSource,
                               public UserItemStateSink,
                               public ArtworkSource,
                               public DownloadSource {
    Q_OBJECT

public:
    // `description` is the source's describe() result.
    PortableProvider(ProviderRegistry *registry, QString accountId, QString label, Capabilities capabilities,
        const QVariantMap& description, QObject *parent = nullptr);
    ~PortableProvider() override;
    void setCapabilities(Capabilities capabilities);

    QString id() const override
    {
        return m_accountId;
    }
    QString displayName() const override
    {
        return m_label;
    }
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
        return m_capabilities.testFlag(Search) ? this : nullptr;
    }
    UserItemStateSink *itemState() override
    {
        return this;
    }
    DownloadSource *downloads() override
    {
        return m_capabilities.testFlag(Downloads) ? this : nullptr;
    }
    QCoro::Task<DownloadPlan> negotiateDownload(DownloadRequest request, QString scope) override;
    QCoro::Task<void> releaseDownload(QVariantMap cleanup) override;
    bool ready() const override
    {
        return true;
    }

    bool signedIn() const override
    {
        return true;
    }
    QString libraryScopeKey() const override
    {
        return m_accountId;
    }
    QCoro::Task<PagedMovieItems> fetchBrowsePage(BrowseDescriptor descriptor, int startIndex, int limit,
        QVariantMap queryOptions, std::optional<QString> cursor) override;
    QCoro::Task<MovieItem> fetchItemDetails(QString itemId) override;
    QCoro::Task<std::vector<MovieItem>> fetchSeasons(QString seriesId) override;
    QCoro::Task<std::vector<MovieItem>> fetchEpisodes(QString seriesId, QString seasonId = {}) override;
    QCoro::Task<std::vector<MovieItem>> fetchResumeItems(int limit = 24) override;
    QCoro::Task<std::vector<MovieItem>> fetchNextUpEpisodes(int limit = 24) override;
    QCoro::Task<std::vector<MovieItem>> fetchLatestItems(QString parentId = {}, int limit = 24) override;
    QCoro::Task<std::vector<MovieItem>> fetchSimilarItems(QString itemId, int limit = 24) override;
    QCoro::Task<std::vector<MovieItem>> fetchRelatedMedia(QString itemId, QString kind) override;
    QCoro::Task<PersonCredits> fetchItemsByPerson(QString personId, int maximumItems = 4000) override;
    QCoro::Task<std::vector<LibraryItem>> fetchLibraries() override;
    QCoro::Task<QVariantMap> fetchLibraryFilterOptions(QString libraryId, QString collectionType = {}) override;
    QCoro::Task<std::vector<MovieItem>> fetchItemsByIds(QStringList itemIds) override;

    QCoro::Task<std::vector<MovieItem>> searchItems(QString searchTerm, int limit = 80) override;
    QCoro::Task<std::vector<MovieItem>> fetchSearchSuggestions(int limit = 20) override;

    QCoro::Task<void> setItemFavorite(QString itemId, bool favorite) override;
    QCoro::Task<void> setItemPlayed(QString itemId, bool played) override;
    QCoro::Task<void> setItemPlaybackPosition(QString itemId, qint64 positionTicks) override;

    QString imageUrl(const ImageRequest& request) const override;

    // Group playback and anything else a provider exposes by name.
    QCoro::Task<QVariantMap> call(QString operation, QVariantMap arguments = {});
    // Quality ceiling and decodable codecs, merged into every resolve call.
    void setPlaybackContext(QVariantMap context);
    // SourceHub supplies one account-filtered immutable snapshot per revision.
    void setPlaybackQueueContext(QVariantMap snapshot, int index);

private:
    class Playback;
    QCoro::Task<ProviderMediaPage> listPage(
        QString operation, QVariantMap arguments, int limit, std::optional<QString> cursor, QString scope = {});
    QCoro::Task<std::vector<MovieItem>> list(
        QString operation, QVariantMap arguments, std::optional<int> limit, QString scope = {});

    ProviderRegistry *m_registry;
    QString m_accountId;
    QString m_label;
    Capabilities m_capabilities;
    QString m_artworkTemplate;
    QVariantMap m_playbackContext;
    QVariantMap m_queueSnapshot;
    int m_queueIndex = -1;
    QFuture<void> m_itemsTail;
    Playback *m_playback = nullptr;
};

} // namespace Spool
