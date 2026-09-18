#pragma once

#include "../media/MediaTypes.h"

#include <QCoroTask>

#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <vector>

namespace JellyfinNative {

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
        BrowseDescriptor descriptor, int startIndex = 0, int limit = 72, QVariantMap queryOptions = {})
        = 0;
    virtual QCoro::Task<MovieItem> fetchItemDetails(QString itemId) = 0;
    virtual QCoro::Task<std::vector<MovieItem>> fetchSeasons(QString seriesId) = 0;
    virtual QCoro::Task<std::vector<MovieItem>> fetchEpisodes(QString seriesId, QString seasonId = {}) = 0;
    virtual QCoro::Task<std::vector<MovieItem>> fetchResumeItems(int limit = 24) = 0;
    virtual QCoro::Task<std::vector<MovieItem>> fetchNextUpEpisodes(int limit = 24) = 0;
    virtual QCoro::Task<std::vector<MovieItem>> fetchLatestItems(QString parentId = {}, int limit = 24) = 0;
    virtual QCoro::Task<std::vector<MovieItem>> fetchSimilarItems(QString itemId, int limit = 24) = 0;
    virtual QCoro::Task<PersonCredits> fetchItemsByPerson(QString personId, int maximumItems = 4000) = 0;

    // The top-level libraries the home page and the rail are built from.
    virtual QCoro::Task<std::vector<LibraryItem>> fetchLibraries() = 0;
    // The genres, years and the like a library can be filtered by; empty
    // when the source offers no filtering.
    virtual QCoro::Task<QVariantMap> fetchLibraryFilterOptions(QString libraryId, QString collectionType = {}) = 0;
    virtual QCoro::Task<std::vector<MovieItem>> fetchItemsByIds(QStringList itemIds) = 0;
};

} // namespace JellyfinNative
