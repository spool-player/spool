#include "app/BrowseSessionController.h"

#include "TestMain.h"

#include <QDebug>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdlib>
#include <utility>

using Spool::BrowseKind;
using Spool::BrowseSessionController;
using Spool::LibraryItem;
using Spool::MediaSourceInfo;
using Spool::MediaStreamInfo;
using Spool::MovieItem;
using Spool::PagedMovieItems;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    qCritical() << message;
    std::exit(EXIT_FAILURE);
}

void requireBrowse(const BrowseSessionController& session, BrowseKind kind, const QString& id, const QString& name,
    const char *message)
{
    const auto descriptor = session.descriptor();
    require(descriptor.kind == kind, message);
    require(descriptor.id == id, message);
    require(descriptor.name == name, message);
}

MovieItem item(QString id, QString title, QString type)
{
    MovieItem result;
    result.id = std::move(id);
    result.title = std::move(title);
    result.itemType = std::move(type);
    return result;
}

} // namespace

SPOOL_TEST_MAIN("browse-session-controller")
{
    QTemporaryDir settingsDir;
    require(settingsDir.isValid(), "temporary settings directory");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings().clear();
    BrowseSessionController session(nullptr);

    LibraryItem library;
    library.id = QStringLiteral("lib");
    library.name = QStringLiteral("Movies");
    library.collectionType = QStringLiteral("movies");
    QVariantMap defaultQuery { { QStringLiteral("sortBy"), QStringLiteral("SortName") } };
    session.enterLibrary(library, QStringLiteral("Movies"), defaultQuery);
    requireBrowse(
        session, BrowseKind::Library, QStringLiteral("lib"), QStringLiteral("Movies"), "library descriptor set");
    require(session.libraryId() == QStringLiteral("lib"), "library id set");
    require(session.query().value(QStringLiteral("sortBy")).toString() == QStringLiteral("SortName"),
        "default library query used");

    QVariantMap dateQuery { { QStringLiteral("sortBy"), QStringLiteral("DateCreated") } };
    require(session.setQuery(dateQuery), "query changed");
    session.enterLibrary(library, QStringLiteral("Movies"), defaultQuery);
    require(session.query().value(QStringLiteral("sortBy")).toString() == QStringLiteral("DateCreated"),
        "library query retained by library id");

    int reloads = 0;
    QObject::connect(&session, &BrowseSessionController::reloadRequested, [&reloads]() { ++reloads; });
    session.setSort(QStringLiteral("DateCreated"), QStringLiteral("Descending"));
    require(session.query().value(QStringLiteral("sortOrder")) == QStringLiteral("Descending"), "sort mutated");
    session.setQueryListValue(QStringLiteral("Genres"), QStringLiteral("Drama"), true);
    require(session.query().value(QStringLiteral("Genres")).toStringList() == QStringList { QStringLiteral("Drama") },
        "list filter added");
    session.setQueryValue(QStringLiteral("IsPlayed"), true);
    require(session.query().value(QStringLiteral("IsPlayed")).toBool(), "bool filter added");
    session.setQueryValue(QStringLiteral("IsPlayed"), {});
    require(!session.query().contains(QStringLiteral("IsPlayed")), "nullable filter removed");
    BrowseSessionController restored(nullptr);
    restored.enterLibrary(library, QStringLiteral("Movies"), defaultQuery);
    require(restored.query().value(QStringLiteral("sortBy")) == QStringLiteral("DateCreated"),
        "sort restored after reopening");
    require(restored.query().value(QStringLiteral("sortOrder")) == QStringLiteral("Descending"),
        "sort direction restored after reopening");
    require(restored.query().value(QStringLiteral("Genres")).toStringList() == QStringList { QStringLiteral("Drama") },
        "filters restored after reopening");
    session.clearFilters();
    require(session.query().size() == 2 && session.query().contains(QStringLiteral("sortBy"))
            && session.query().contains(QStringLiteral("sortOrder")),
        "clear filters preserves sort");
    require(reloads == 5, "each query mutation requests one reload");

    const MovieItem series = item(QStringLiteral("series"), QStringLiteral("Show"), QStringLiteral("Series"));
    require(!session.enterItem(series), "series detail is not represented as a generic browse page");

    session.enterNamedCollection(QStringLiteral("genre"), QStringLiteral("Drama"));
    require(session.descriptor().kind == BrowseKind::Genre, "genre descriptor set");
    session.enterNamedCollection(QStringLiteral("genre"), QStringLiteral("Post-Rock"), QStringLiteral("music"));
    require(session.descriptor().kind == BrowseKind::Genre
            && session.descriptor().collectionType == QStringLiteral("music")
            && session.libraryCollectionType() == QStringLiteral("music"),
        "music genre retains its collection type");
    session.enterNamedCollection(QStringLiteral("studio"), QStringLiteral("Studio"));
    require(session.descriptor().kind == BrowseKind::Studio, "studio descriptor set");

    require(session.enterItem(item(QStringLiteral("playlist"), QStringLiteral("Queue"), QStringLiteral("Playlist"))),
        "playlist accepted as browse item");
    requireBrowse(
        session, BrowseKind::Playlist, QStringLiteral("playlist"), QStringLiteral("Queue"), "playlist descriptor set");
    require(session.enterItem(item(QStringLiteral("box"), QStringLiteral("Collection"), QStringLiteral("BoxSet"))),
        "box set accepted as browse item");
    requireBrowse(
        session, BrowseKind::BoxSet, QStringLiteral("box"), QStringLiteral("Collection"), "box set descriptor set");
    require(session.enterItem(item(QStringLiteral("folder"), QStringLiteral("Folder"), QStringLiteral("Folder"))),
        "folder accepted as browse item");
    requireBrowse(session, BrowseKind::FolderChildren, QStringLiteral("folder"), QStringLiteral("Folder"),
        "folder descriptor set");

    PagedMovieItems page;
    page.items = { item(QStringLiteral("movie"), QStringLiteral("Movie"), QStringLiteral("Movie")) };
    page.totalRecordCount = 3;
    page.startIndex = 0;
    page.limit = 1;
    session.setPage(page, QStringLiteral("cache"), false);
    require(session.items()->count() == 1, "page stored one item");
    require(session.hasMore(), "page tracks remaining items");
    require(session.nextStartIndex() == 1, "next start index updated");
    int moreRequests = 0;
    QObject::connect(&session, &BrowseSessionController::moreItemsRequested, [&moreRequests]() { ++moreRequests; });
    session.prefetchVisibleRange(0, 0);
    require(moreRequests == 1, "showing the first page prefetches the second page");
    session.setLoadingMore(true);
    session.prefetchNextPage();
    session.prefetchVisibleRange(0, 0);
    require(moreRequests == 1, "active page request is not duplicated");
    session.setLoadingMore(false);

    PagedMovieItems secondPage;
    secondPage.items = { item(QStringLiteral("movie-2"), QStringLiteral("Movie 2"), QStringLiteral("Movie")) };
    secondPage.totalRecordCount = 3;
    secondPage.startIndex = 1;
    secondPage.limit = 1;
    session.setPage(secondPage, QStringLiteral("cache"), true);
    require(session.items()->count() == 2, "second page appended its item");
    require(session.nextStartIndex() == 2, "second page advanced the next start index");
    require(session.hasMore(), "second page retained the remaining-page state");
    session.prefetchVisibleRange(0, 0);
    require(moreRequests == 2, "third page is requested before entering the second page");
    session.setLoadingMore(true);
    session.prefetchVisibleRange(1, 1);
    require(moreRequests == 2, "rolling prefetch does not duplicate an active third-page request");
    session.setLoadingMore(false);

    PagedMovieItems finalPage;
    finalPage.items = { item(QStringLiteral("movie-3"), QStringLiteral("Movie 3"), QStringLiteral("Movie")) };
    finalPage.totalRecordCount = 3;
    finalPage.startIndex = 2;
    finalPage.limit = 1;
    session.setPage(finalPage, QStringLiteral("cache"), true);
    require(session.items()->count() == 3, "final page appended its item");
    require(session.nextStartIndex() == 3, "final page advanced to the server total");
    require(!session.hasMore(), "final page stopped pagination");
    session.prefetchNextPage();
    require(moreRequests == 2, "hold prefetch stops after the final page");

    PagedMovieItems largeFirstPage;
    largeFirstPage.totalRecordCount = 300;
    largeFirstPage.startIndex = 0;
    largeFirstPage.limit = 100;
    PagedMovieItems largeSecondPage = largeFirstPage;
    largeSecondPage.startIndex = 100;
    for (int index = 0; index < 100; ++index) {
        largeFirstPage.items.push_back(
            item(QStringLiteral("large-%1").arg(index), QStringLiteral("Movie"), QStringLiteral("Movie")));
        largeSecondPage.items.push_back(
            item(QStringLiteral("large-%1").arg(index + 100), QStringLiteral("Movie"), QStringLiteral("Movie")));
    }
    session.setPage(largeFirstPage, QStringLiteral("large-cache"), false);
    session.setPage(largeSecondPage, QStringLiteral("large-cache"), true);
    require(session.items()->count() == 200, "large second page appended without replacing loaded rows");
    session.prefetchPageForIndex(49);
    require(moreRequests == 2, "pagination waits until the configured headroom boundary");
    session.prefetchPageForIndex(50);
    require(moreRequests == 3, "pagination starts with one and a half pages of headroom");
    session.setLoadingMore(true);
    session.prefetchPageForIndex(150);
    require(moreRequests == 3, "continuous page checks do not duplicate the active request");
    session.setLoadingMore(false);
    MovieItem metadataItem = item(QStringLiteral("metadata"), QStringLiteral("Metadata"), QStringLiteral("Movie"));
    MediaStreamInfo undefinedAudio;
    undefinedAudio.type = QStringLiteral("Audio");
    undefinedAudio.language = QStringLiteral("und");
    undefinedAudio.channels = 8;
    undefinedAudio.isDefault = true;
    MediaSourceInfo metadataSource;
    metadataSource.streams = { undefinedAudio };
    metadataItem.mediaSources = { metadataSource };
    PagedMovieItems metadataPage;
    metadataPage.items = { metadataItem };
    metadataPage.totalRecordCount = 1;
    session.setPage(metadataPage, QStringLiteral("metadata-cache"), false);
    require(session.mediaInfoFor(0, {}).value(QStringLiteral("audio")).toString() == QStringLiteral("7.1"),
        "undefined audio language omitted while channel layout remains");

    session.reset();
    require(!session.descriptor().isValid(), "reset clears descriptor");
    require(session.items()->count() == 0, "reset clears items");

    return EXIT_SUCCESS;
}
