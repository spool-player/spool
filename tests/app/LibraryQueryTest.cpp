#include "app/LibraryQuery.h"

#include "TestMain.h"

#include <QDebug>

#include <cstdlib>
#include <utility>

using Spool::activeLibraryFilterCount;
using Spool::defaultLibraryQuery;
using Spool::libraryCacheKey;
using Spool::LibraryItem;
using Spool::libraryQueryStringList;
using Spool::supportsLatestLibraryRow;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    qCritical() << message;
    std::exit(EXIT_FAILURE);
}

LibraryItem library(QString id, QString name, QString collectionType)
{
    LibraryItem item;
    item.id = std::move(id);
    item.name = std::move(name);
    item.collectionType = std::move(collectionType);
    return item;
}

} // namespace

SPOOL_TEST_MAIN("library-query")
{
    const LibraryItem movies = library(QStringLiteral("movies-id"), QStringLiteral("Films"), QStringLiteral("movies"));
    const LibraryItem series = library(QStringLiteral("series-id"), QStringLiteral("Shows"), QStringLiteral("tvshows"));
    const LibraryItem photos = library(QStringLiteral("photos-id"), QStringLiteral("Photos"), QStringLiteral("photos"));

    require(libraryCacheKey(movies) == QStringLiteral("movies-id"), "movie library cache key changed");
    require(libraryCacheKey(series) == QStringLiteral("series/series-id"), "series library cache key changed");
    require(libraryCacheKey(photos) == QStringLiteral("library/photos/photos-id"), "generic library cache key changed");
    require(supportsLatestLibraryRow(movies), "movie library should support latest rows");
    require(supportsLatestLibraryRow(photos), "generic content library should support latest rows");
    require(!supportsLatestLibraryRow(library(QStringLiteral(""), QStringLiteral("Empty"), QStringLiteral("movies"))),
        "empty library id should not support latest rows");
    require(!supportsLatestLibraryRow(
                library(QStringLiteral("playlists-id"), QStringLiteral("Playlists"), QStringLiteral("playlists"))),
        "playlist library should not support latest rows");

    const QVariantMap defaults = defaultLibraryQuery(movies);
    require(libraryCacheKey(movies, defaults) == QStringLiteral("movies-id"),
        "default query should not be part of the cache key");
    require(activeLibraryFilterCount(defaults) == 0, "default sort should not count as an active filter");

    QVariantMap filtered = defaults;
    filtered.insert(QStringLiteral("genres"), QStringList { QStringLiteral("Drama"), QStringLiteral("Comedy") });
    filtered.insert(QStringLiteral("is4K"), true);
    filtered.insert(QStringLiteral("hasSubtitles"), false);
    filtered.insert(QStringLiteral("alphabet"), QStringLiteral("B"));
    require(
        activeLibraryFilterCount(filtered) == 4, "active filter count did not include list and truthy scalar filters");

    require(libraryQueryStringList(filtered, QStringLiteral("genres")).join(QLatin1Char(','))
            == QStringLiteral("Drama,Comedy"),
        "QStringList query values were not retained");

    QVariantMap collectionFilter = defaults;
    collectionFilter.insert(QStringLiteral("includeItemTypes"), QStringList { QStringLiteral("BoxSet") });
    require(activeLibraryFilterCount(collectionFilter) == 1,
        "collection item type filter did not count as an active filter");
    require(libraryCacheKey(movies, collectionFilter)
            == QStringLiteral("movies-id?includeItemTypes=BoxSet&"
                              "sortBy=SortName&sortOrder=Ascending"),
        "collection item type filter did not participate in the cache key");

    QVariantMap reordered = defaults;
    reordered.insert(QStringLiteral("is4K"), true);
    reordered.insert(QStringLiteral("alphabet"), QStringLiteral("B"));
    reordered.insert(QStringLiteral("genres"), QStringList { QStringLiteral("Comedy"), QStringLiteral("Drama") });
    const QString firstKey = libraryCacheKey(movies, filtered);
    const QString secondKey = libraryCacheKey(movies, reordered);
    require(firstKey == secondKey, "query cache signature was not stable across map/list ordering");
    require(firstKey
            == QStringLiteral("movies-id?alphabet=B&genres=Comedy,Drama&"
                              "is4K=true&sortBy=SortName&sortOrder=Ascending"),
        "query cache signature changed unexpectedly");

    return EXIT_SUCCESS;
}
