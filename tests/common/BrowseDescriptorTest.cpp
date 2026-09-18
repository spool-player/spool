#include "media/MediaTypes.h"

#include "TestMain.h"

#include <QDebug>

#include <cstdlib>

using JellyfinNative::BrowseDescriptor;
using JellyfinNative::BrowseKind;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    qCritical() << message;
    std::exit(EXIT_FAILURE);
}

void requireDescriptor(const BrowseDescriptor& descriptor, BrowseKind kind, const QString& id, const QString& name,
    const QString& collectionType, const QString& seriesId, const QString& seasonId, const char *message)
{
    require(descriptor.kind == kind, message);
    require(descriptor.id == id, message);
    require(descriptor.name == name, message);
    require(descriptor.collectionType == collectionType, message);
    require(descriptor.seriesId == seriesId, message);
    require(descriptor.seasonId == seasonId, message);
}

} // namespace

JELLYFIN_TEST_MAIN("browse-descriptor")
{
    const BrowseDescriptor library
        = BrowseDescriptor::library(QStringLiteral("library-1"), QStringLiteral("movies"), QStringLiteral("Movies"));
    requireDescriptor(library, BrowseKind::Library, QStringLiteral("library-1"), QStringLiteral("Movies"),
        QStringLiteral("movies"), QString(), QString(), "library constructor did not preserve identity");

    const BrowseDescriptor folder
        = BrowseDescriptor::folderChildren(QStringLiteral("folder-1"), QStringLiteral("Folder"));
    requireDescriptor(folder, BrowseKind::FolderChildren, QStringLiteral("folder-1"), QStringLiteral("Folder"),
        QString(), QString(), QString(), "folder constructor did not preserve identity");

    const BrowseDescriptor genre = BrowseDescriptor::genre(QStringLiteral("Film Noir"));
    requireDescriptor(genre, BrowseKind::Genre, QString(), QStringLiteral("Film Noir"), QString(), QString(), QString(),
        "genre constructor did not preserve name");

    const BrowseDescriptor studio = BrowseDescriptor::studio(QStringLiteral("Warner Bros"));
    requireDescriptor(studio, BrowseKind::Studio, QString(), QStringLiteral("Warner Bros"), QString(), QString(),
        QString(), "studio constructor did not preserve name");

    const BrowseDescriptor series
        = BrowseDescriptor::seriesSeasons(QStringLiteral("series-1"), QStringLiteral("Series"));
    requireDescriptor(series, BrowseKind::SeriesSeasons, QStringLiteral("series-1"), QStringLiteral("Series"),
        QString(), QStringLiteral("series-1"), QString(), "series constructor did not preserve series identity");

    const BrowseDescriptor season = BrowseDescriptor::seasonEpisodes(
        QStringLiteral("series-1"), QStringLiteral("season-1"), QStringLiteral("Season 1"));
    requireDescriptor(season, BrowseKind::SeasonEpisodes, QStringLiteral("season-1"), QStringLiteral("Season 1"),
        QString(), QStringLiteral("series-1"), QStringLiteral("season-1"),
        "season constructor did not preserve season identity");

    const BrowseDescriptor seriesEpisodes
        = BrowseDescriptor::seasonEpisodes(QStringLiteral("series-1"), QString(), QStringLiteral("Episodes"));
    requireDescriptor(seriesEpisodes, BrowseKind::SeasonEpisodes, QStringLiteral("series-1"),
        QStringLiteral("Episodes"), QString(), QStringLiteral("series-1"), QString(),
        "series episodes constructor did not fall back to series id");

    const BrowseDescriptor playlist
        = BrowseDescriptor::playlist(QStringLiteral("playlist-1"), QStringLiteral("Playlist"));
    requireDescriptor(playlist, BrowseKind::Playlist, QStringLiteral("playlist-1"), QStringLiteral("Playlist"),
        QString(), QString(), QString(), "playlist constructor did not preserve identity");

    const BrowseDescriptor boxSet = BrowseDescriptor::boxSet(QStringLiteral("boxset-1"), QStringLiteral("Box Set"));
    requireDescriptor(boxSet, BrowseKind::BoxSet, QStringLiteral("boxset-1"), QStringLiteral("Box Set"), QString(),
        QString(), QString(), "box set constructor did not preserve identity");

    require(library.isValid(), "library descriptor should be valid with an id");
    require(!BrowseDescriptor::library(QString(), QStringLiteral("movies")).isValid(),
        "library descriptor without an id should be invalid");
    require(
        !BrowseDescriptor::folderChildren(QString()).isValid(), "folder descriptor without an id should be invalid");
    require(!BrowseDescriptor::playlist(QString()).isValid(), "playlist descriptor without an id should be invalid");
    require(!BrowseDescriptor::boxSet(QString()).isValid(), "box set descriptor without an id should be invalid");
    require(!BrowseDescriptor::genre(QStringLiteral("  ")).isValid(), "blank genre descriptor should be invalid");
    require(!BrowseDescriptor::studio(QStringLiteral("\t")).isValid(), "blank studio descriptor should be invalid");
    require(seriesEpisodes.isValid(), "series episodes descriptor should be valid without a season id");
    require(!BrowseDescriptor::seasonEpisodes(QString(), QStringLiteral("season-1")).isValid(),
        "season descriptor without a series id should be invalid");
    require(!BrowseDescriptor().isValid(), "empty descriptor should not be valid for browsing");

    require(library.cacheKey() == QStringLiteral("library/library-1/movies"),
        "library cache key should include library id and collection type");
    require(genre.cacheKey() == QStringLiteral("genre/Film Noir"),
        "genre cache key should use the browsed name when there is no id");
    require(studio.cacheKey() == QStringLiteral("studio/Warner Bros"),
        "studio cache key should use the browsed name when there is no id");
    require(series.cacheKey() == QStringLiteral("seriesSeasons/series/series-1"),
        "series cache key should include the series namespace");
    require(season.cacheKey() == QStringLiteral("seasonEpisodes/series/series-1/season-1"),
        "season cache key should include series and season ids");
    require(seriesEpisodes.cacheKey() == QStringLiteral("seasonEpisodes/series/series-1"),
        "series episode cache key should use the series id without a season id");
    require(folder.cacheKey() == QStringLiteral("folderChildren/folder-1"),
        "folder cache key should include the folder id");
    require(playlist.cacheKey() == QStringLiteral("playlist/playlist-1"),
        "playlist cache key should include the playlist id");
    require(boxSet.cacheKey() == QStringLiteral("boxset/boxset-1"), "box set cache key should include the box set id");

    const QVariantMap query { { QStringLiteral("sortOrder"), QStringLiteral("Descending") },
        { QStringLiteral("sortBy"), QStringLiteral("SortName") } };
    require(library.cacheKey(query) == QStringLiteral("library/library-1/movies?sortBy=SortName&sortOrder=Descending"),
        "cache key should include a deterministic query signature");

    return EXIT_SUCCESS;
}
