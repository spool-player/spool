#pragma once

#include "../media/MediaTypes.h"

#include <QCoroTask>

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <optional>
#include <vector>

namespace Spool {

// The browse and lookup half of a media source: what the content, home and
// prefetch controllers ask for today, and nothing more. It is a plain
// abstract class rather than a QObject because the Jellyfin facade already
// derives from PlaybackSource (a QObject) and moc allows only one; a source
// implements this beside that, and QObject-ness is never needed here.
class Catalog {
public:
    virtual ~Catalog() = default;

    // Whether lookups can be made right now; a source that needs a session
    // reports false until it has one.
    virtual bool signedIn() const = 0;
    // Names the account and library this catalog serves so cached payloads
    // for one never surface under another. Empty while nothing is served.
    virtual QString libraryScopeKey() const = 0;

    virtual QCoro::Task<PagedMovieItems> fetchBrowsePage(
        BrowseDescriptor descriptor, int startIndex, int limit, QVariantMap queryOptions, std::optional<QString> cursor)
        = 0;
    virtual QCoro::Task<MovieItem> fetchItemDetails(QString itemId) = 0;
    virtual QCoro::Task<std::vector<MovieItem>> fetchSeasons(QString seriesId) = 0;
    virtual QCoro::Task<std::vector<MovieItem>> fetchEpisodes(QString seriesId, QString seasonId = {}) = 0;
    virtual QCoro::Task<std::vector<MovieItem>> fetchResumeItems(int limit = 24) = 0;
    virtual QCoro::Task<std::vector<MovieItem>> fetchNextUpEpisodes(int limit = 24) = 0;
    virtual QCoro::Task<std::vector<MovieItem>> fetchLatestItems(QString parentId = {}, int limit = 24) = 0;
    virtual QCoro::Task<std::vector<MovieItem>> fetchSimilarItems(QString itemId, int limit = 24) = 0;
    // Server-indexed trailers ("trailers") and extras ("extras") for an item,
    // as ordinary playable rows. Optional: a source without related media
    // returns an empty list.
    virtual QCoro::Task<std::vector<MovieItem>> fetchRelatedMedia(QString itemId, QString kind) = 0;
    virtual QCoro::Task<PersonCredits> fetchItemsByPerson(QString personId, int maximumItems = 4000) = 0;

    // The top-level libraries the home page and the rail are built from.
    virtual QCoro::Task<std::vector<LibraryItem>> fetchLibraries() = 0;
    // The genres, years and the like a library can be filtered by; empty
    // when the source offers no filtering.
    virtual QCoro::Task<QVariantMap> fetchLibraryFilterOptions(QString libraryId, QString collectionType = {}) = 0;
    virtual QCoro::Task<std::vector<MovieItem>> fetchItemsByIds(QStringList itemIds) = 0;

    // Locally observed user state outranks asynchronously hydrated snapshots
    // until this catalog session/account is reset. Resume writes do not change
    // watched state: a short replay of a watched item remains watched.
    void recordLocalResumeTicks(const QString& itemId, qint64 positionTicks)
    {
        if (!itemId.isEmpty() && positionTicks >= 0)
            m_localPlaybackState[itemId].resumeTicks = positionTicks;
    }
    void recordLocalPlayed(const QString& itemId, bool played)
    {
        if (!itemId.isEmpty())
            m_localPlaybackState.insert(itemId, { 0, played });
    }
    bool applyLocalPlaybackState(MovieItem& item) const
    {
        const auto state = m_localPlaybackState.constFind(item.id);
        if (state == m_localPlaybackState.cend())
            return false;
        item.resumeTicks = normalizedResumeTicks(state->resumeTicks, item.runtimeTicks);
        if (state->played)
            item.played = *state->played;
        return true;
    }
    void clearLocalPlaybackState(const QString& itemIdPrefix = {})
    {
        if (itemIdPrefix.isEmpty()) {
            m_localPlaybackState.clear();
            return;
        }
        for (auto it = m_localPlaybackState.begin(); it != m_localPlaybackState.end();) {
            if (it.key().startsWith(itemIdPrefix))
                it = m_localPlaybackState.erase(it);
            else
                ++it;
        }
    }

private:
    struct LocalPlaybackState {
        qint64 resumeTicks = 0;
        std::optional<bool> played;
    };
    QHash<QString, LocalPlaybackState> m_localPlaybackState;
};

} // namespace Spool
