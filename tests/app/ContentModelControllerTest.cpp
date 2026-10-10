#include "app/ContentModelController.h"
#include "app/BrowseSessionController.h"
#include "app/HomeModelController.h"
#include "app/LibraryPrefetchController.h"
#include "app/LibraryQuery.h"
#include "app/SearchController.h"
#include "app/UserItemStateController.h"
#include "common/AsyncTask.h"
#include "common/MetaJson.h"
#include "provider/Catalog.h"
#include "provider/SearchSource.h"

#include "TestMain.h"
#include "TestRequire.h"

#include <QCoreApplication>
#include <QCoroFuture>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPromise>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using Spool::BrowseDescriptor;
using Spool::BrowseKind;
using Spool::Catalog;
using Spool::ContentModelController;
using Spool::HomeModelController;
using Spool::LibraryItem;
using Spool::LibraryPrefetchController;
using Spool::MovieGridModel;
using Spool::MovieItem;
using Spool::PagedMovieItems;
using Spool::PersonCredits;
using Spool::SearchController;
using Spool::SearchSource;

namespace {

using SpoolTests::require;

LibraryItem makeLibrary(const QString& id, const QString& name, const QString& collectionType)
{
    LibraryItem item;
    item.id = id;
    item.name = name;
    item.collectionType = collectionType;
    return item;
}

class TestCatalog final : public Catalog, public SearchSource {
public:
    std::optional<std::vector<MovieItem>> episodeRows;
    QString scopeKey = QStringLiteral("test-scope");
    std::vector<MovieItem> resumeRows;
    std::vector<MovieItem> nextUpRows;
    QHash<QString, std::vector<MovieItem>> latestRows;
    std::shared_ptr<QPromise<std::vector<MovieItem>>> pendingResume;
    std::shared_ptr<QPromise<MovieItem>> pendingDetails;
    QHash<QString, std::shared_ptr<QPromise<std::vector<MovieItem>>>> pendingLatest;
    int completedHomeRequests = 0;
    bool signedIn() const override
    {
        return true;
    }

    QString libraryScopeKey() const override
    {
        return scopeKey;
    }

    QCoro::Task<PagedMovieItems> fetchBrowsePage(BrowseDescriptor descriptor, int startIndex, int limit,
        QVariantMap queryOptions, std::optional<QString>) override
    {
        Q_UNUSED(startIndex);
        Q_UNUSED(limit);
        PagedMovieItems page;
        if (descriptor.kind == BrowseKind::Playlist) {
            MovieItem item;
            item.id = QStringLiteral("playlist-movie-1");
            item.title = QStringLiteral("Playlist Movie");
            item.itemType = QStringLiteral("Movie");
            item.playlistItemId = QStringLiteral("playlist-item-1");
            page.items.push_back(std::move(item));
            page.totalRecordCount = 1;
        } else if (descriptor.id == QStringLiteral("boxset-1")) {
            MovieItem item;
            item.id = QStringLiteral("boxset-child-1");
            item.title = QStringLiteral("BoxSet Child");
            item.itemType = QStringLiteral("Movie");
            page.items.push_back(std::move(item));
            page.totalRecordCount = 1;
        } else {
            const QStringList types = queryOptions.value(QStringLiteral("includeItemTypes")).toStringList();
            if (types.contains(QStringLiteral("BoxSet"))) {
                MovieItem item;
                item.id = QStringLiteral("boxset-1");
                item.title = QStringLiteral("Collection One");
                item.itemType = QStringLiteral("BoxSet");
                page.items.push_back(std::move(item));
                page.totalRecordCount = 1;
            }
        }
        co_return page;
    }

    QCoro::Task<MovieItem> fetchItemDetails(QString itemId) override
    {
        if (pendingDetails) {
            auto future = pendingDetails->future();
            auto awaitable = qCoro(future);
            co_return co_await awaitable.takeResult();
        }
        MovieItem item;
        item.id = itemId;
        item.title = QStringLiteral("Episode One");
        item.itemType = QStringLiteral("Episode");
        item.seriesId = QStringLiteral("series-1");
        item.seasonId = QStringLiteral("season-1");
        item.runtimeTicks = 1200LL * 10'000'000;
        co_return item;
    }

    QCoro::Task<std::vector<MovieItem>> fetchSeasons(QString seriesId) override
    {
        Q_UNUSED(seriesId);
        MovieItem season;
        season.id = QStringLiteral("season-1");
        season.title = QStringLiteral("Season 2");
        season.itemType = QStringLiteral("Season");
        season.seriesId = QStringLiteral("series-1");
        season.seriesName = QStringLiteral("Series One");
        season.seasonNumber = 2;
        co_return std::vector<MovieItem> { season };
    }

    QCoro::Task<std::vector<MovieItem>> fetchEpisodes(QString seriesId, QString seasonId = {}) override
    {
        Q_UNUSED(seriesId);
        Q_UNUSED(seasonId);
        if (episodeRows)
            co_return std::vector<MovieItem>(*episodeRows);
        MovieItem episode;
        episode.id = QStringLiteral("episode-row");
        episode.title = QStringLiteral("The Loaded Episode");
        episode.itemType = QStringLiteral("Episode");
        episode.seriesId = QStringLiteral("series-1");
        episode.seasonId = QStringLiteral("season-1");
        episode.seriesName = QStringLiteral("Series One");
        episode.seriesPrimaryImageTag = QStringLiteral("series-primary-tag");
        episode.seasonNumber = 2;
        episode.episodeNumber = 7;
        co_return std::vector<MovieItem> { episode };
    }

    QCoro::Task<std::vector<MovieItem>> fetchResumeItems(int limit = 24) override
    {
        Q_UNUSED(limit);
        if (pendingResume) {
            auto future = pendingResume->future();
            auto awaitable = qCoro(future);
            auto rows = co_await awaitable.takeResult();
            ++completedHomeRequests;
            co_return rows;
        }
        co_return resumeRows;
    }

    QCoro::Task<std::vector<MovieItem>> fetchNextUpEpisodes(int limit = 24) override
    {
        Q_UNUSED(limit);
        co_return nextUpRows;
    }

    QCoro::Task<std::vector<MovieItem>> fetchLatestItems(QString parentId = {}, int limit = 24) override
    {
        Q_UNUSED(limit);
        if (const auto pending = pendingLatest.value(parentId)) {
            auto future = pending->future();
            auto awaitable = qCoro(future);
            auto rows = co_await awaitable.takeResult();
            ++completedHomeRequests;
            co_return rows;
        }
        if (latestRows.contains(parentId))
            co_return latestRows.value(parentId);
        std::vector<MovieItem> items;
        if (parentId == QStringLiteral("shows-id")) {
            for (int i = 1; i <= 145; ++i) {
                MovieItem ep;
                ep.id = QStringLiteral("episode-%1").arg(i);
                ep.title = QStringLiteral("Episode %1").arg(i);
                ep.itemType = QStringLiteral("Episode");
                ep.seriesId = QStringLiteral("series-1");
                ep.seasonId = QStringLiteral("season-1");
                ep.seriesName = QStringLiteral("Series One");
                ep.seriesPrimaryImageTag = QStringLiteral("series-primary-tag");
                ep.seasonNumber = 2;
                ep.episodeNumber = i;
                items.push_back(std::move(ep));
            }
        } else if (parentId == QStringLiteral("single-show-id")) {
            MovieItem ep;
            ep.id = QStringLiteral("single-ep-1");
            ep.title = QStringLiteral("Episode 1");
            ep.itemType = QStringLiteral("Episode");
            ep.seriesId = QStringLiteral("series-1");
            ep.seasonId = QStringLiteral("season-1");
            ep.seriesName = QStringLiteral("Series One");
            ep.seriesPrimaryImageTag = QStringLiteral("series-primary-tag");
            ep.seasonNumber = 1;
            ep.episodeNumber = 1;
            items.push_back(std::move(ep));
        } else if (parentId == QStringLiteral("photos-id")) {
            MovieItem photo;
            photo.id = QStringLiteral("photo-1");
            photo.title = QStringLiteral("Photo One");
            photo.itemType = QStringLiteral("Photo");
            items.push_back(std::move(photo));
        }
        co_return items;
    }

    QCoro::Task<std::vector<MovieItem>> fetchSimilarItems(QString itemId, int limit = 24) override
    {
        Q_UNUSED(itemId);
        Q_UNUSED(limit);
        co_return std::vector<MovieItem> {};
    }

    QCoro::Task<std::vector<MovieItem>> fetchRelatedMedia(QString itemId, QString kind) override
    {
        Q_UNUSED(itemId);
        Q_UNUSED(kind);
        co_return std::vector<MovieItem> {};
    }

    QCoro::Task<PersonCredits> fetchItemsByPerson(QString personId, int maximumItems = 4000) override
    {
        Q_UNUSED(personId);
        Q_UNUSED(maximumItems);
        PersonCredits credits;
        credits.items.reserve(207);
        for (int i = 0; i < 200; ++i) {
            MovieItem m;
            m.id = QStringLiteral("person-movie-%1").arg(i);
            m.title = QStringLiteral("Movie %1").arg(i, 3, 10, QLatin1Char('0'));
            m.itemType = QStringLiteral("Movie");
            credits.items.push_back(std::move(m));
        }

        MovieItem directEp;
        directEp.id = QStringLiteral("direct-episode");
        directEp.title = QStringLiteral("Direct Episode");
        directEp.itemType = QStringLiteral("Episode");
        directEp.seriesId = QStringLiteral("series-direct");
        directEp.seasonId = QStringLiteral("series-direct-season-1");
        directEp.seriesName = QStringLiteral("Direct Show");
        directEp.seasonNumber = 1;
        directEp.episodeNumber = 1;
        credits.items.push_back(std::move(directEp));

        MovieItem guest1;
        guest1.id = QStringLiteral("guest-1");
        guest1.title = QStringLiteral("Guest Episode 1");
        guest1.itemType = QStringLiteral("Episode");
        guest1.seriesId = QStringLiteral("series-guest");
        guest1.seasonId = QStringLiteral("series-guest-season-2");
        guest1.seriesName = QStringLiteral("Guest Show");
        guest1.seasonNumber = 2;
        guest1.episodeNumber = 1;
        credits.items.push_back(std::move(guest1));

        MovieItem guest2;
        guest2.id = QStringLiteral("guest-2");
        guest2.title = QStringLiteral("Guest Episode 2");
        guest2.itemType = QStringLiteral("Episode");
        guest2.seriesId = QStringLiteral("series-guest");
        guest2.seasonId = QStringLiteral("series-guest-season-2");
        guest2.seriesName = QStringLiteral("Guest Show");
        guest2.seasonNumber = 2;
        guest2.episodeNumber = 2;
        credits.items.push_back(std::move(guest2));

        for (int i = 1; i <= 3; ++i) {
            MovieItem maj;
            maj.id = QStringLiteral("majority-%1").arg(i);
            maj.title = QStringLiteral("Majority Episode %1").arg(i);
            maj.itemType = QStringLiteral("Episode");
            maj.seriesId = QStringLiteral("series-majority");
            maj.seasonId = QStringLiteral("series-majority-season-1");
            maj.seriesName = QStringLiteral("Majority Show");
            maj.seasonNumber = 1;
            maj.episodeNumber = i;
            credits.items.push_back(std::move(maj));
        }

        MovieItem directSeries;
        directSeries.id = QStringLiteral("series-direct");
        directSeries.title = QStringLiteral("Direct Show");
        directSeries.itemType = QStringLiteral("Series");
        directSeries.recursiveItemCount = 100;
        credits.items.push_back(directSeries);
        credits.relatedSeries.push_back(directSeries);

        MovieItem majoritySeries;
        majoritySeries.id = QStringLiteral("series-majority");
        majoritySeries.title = QStringLiteral("Majority Show");
        majoritySeries.itemType = QStringLiteral("Series");
        majoritySeries.recursiveItemCount = 5;
        credits.relatedSeries.push_back(std::move(majoritySeries));

        co_return credits;
    }

    QCoro::Task<std::vector<LibraryItem>> fetchLibraries() override
    {
        co_return std::vector<LibraryItem> {};
    }

    QCoro::Task<QVariantMap> fetchLibraryFilterOptions(QString libraryId, QString collectionType = {}) override
    {
        Q_UNUSED(libraryId);
        Q_UNUSED(collectionType);
        co_return QVariantMap {};
    }

    QCoro::Task<std::vector<MovieItem>> fetchItemsByIds(QStringList itemIds) override
    {
        Q_UNUSED(itemIds);
        co_return std::vector<MovieItem> {};
    }

    // SearchSource
    QCoro::Task<std::vector<MovieItem>> searchItems(QString searchTerm, int limit = 80) override
    {
        Q_UNUSED(searchTerm);
        Q_UNUSED(limit);
        std::vector<MovieItem> results;

        MovieItem movie;
        movie.id = QStringLiteral("movie-1");
        movie.title = QStringLiteral("Movie One");
        movie.itemType = QStringLiteral("Movie");
        results.push_back(std::move(movie));

        MovieItem series;
        series.id = QStringLiteral("series-1");
        series.title = QStringLiteral("Series One");
        series.itemType = QStringLiteral("Series");
        results.push_back(std::move(series));

        MovieItem episode;
        episode.id = QStringLiteral("episode-row");
        episode.title = QStringLiteral("The Loaded Episode");
        episode.itemType = QStringLiteral("Episode");
        episode.seriesId = QStringLiteral("series-1");
        episode.seasonId = QStringLiteral("season-1");
        episode.seriesName = QStringLiteral("Series One");
        episode.seriesPrimaryImageTag = QStringLiteral("series-primary-tag");
        episode.seasonNumber = 2;
        episode.episodeNumber = 7;
        results.push_back(std::move(episode));

        MovieItem photo;
        photo.id = QStringLiteral("photo-1");
        photo.title = QStringLiteral("Photo One");
        photo.itemType = QStringLiteral("Photo");
        results.push_back(std::move(photo));

        co_return results;
    }

    QCoro::Task<std::vector<MovieItem>> fetchSearchSuggestions(int limit = 20) override
    {
        Q_UNUSED(limit);
        co_return std::vector<MovieItem> {};
    }
};

bool waitForDetailRowsIdle(ContentModelController& controller, int timeoutMs)
{
    if (!controller.detailRowsBusy())
        return true;

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&controller, &ContentModelController::detailRowsChanged, &loop, [&]() {
        if (!controller.detailRowsBusy())
            loop.quit();
    });

    timeout.start(timeoutMs);
    loop.exec();
    return !controller.detailRowsBusy();
}

bool waitForBrowsePage(Catalog& catalog, const BrowseDescriptor& descriptor, const QVariantMap& queryOptions,
    PagedMovieItems& page, QString& error, int timeoutMs)
{
    bool finished = false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    Spool::Async::runDetached(
        catalog.fetchBrowsePage(descriptor, 0, 72, queryOptions, std::nullopt),
        [&page, &finished, &loop](PagedMovieItems value) {
            page = std::move(value);
            finished = true;
            loop.quit();
        },
        [&error, &finished, &loop](const std::exception_ptr& exception) {
            error = Spool::exceptionMessage(exception);
            finished = true;
            loop.quit();
        },
        "test fetchBrowsePage");

    timeout.start(timeoutMs);
    if (!finished)
        loop.exec();
    return finished && error.isEmpty();
}

bool waitForSearch(SearchController& search, int timeoutMs)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&search, &SearchController::resultsChanged, &loop, &QEventLoop::quit);
    search.search(QStringLiteral("mixed"));
    timeout.start(timeoutMs);
    if (search.busy())
        loop.exec();
    return !search.busy() && search.resultCount() == 4;
}

void waitUntil(const std::function<bool()>& condition, const char *message)
{
    QElapsedTimer timeout;
    timeout.start();
    while (!condition() && timeout.elapsed() < 2000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(condition(), message);
}

bool waitForHomeRows(HomeModelController& home, int timeoutMs)
{
    if (home.latestLibraryRows().size() == 3)
        return true;

    bool changed = false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&home, &HomeModelController::latestLibraryRowsChanged, &loop, [&]() {
        changed = true;
        loop.quit();
    });
    timeout.start(timeoutMs);
    loop.exec();
    return changed || home.latestLibraryRows().size() == 3;
}

bool waitForPersonRows(ContentModelController& controller, int timeoutMs)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&controller, &ContentModelController::personItemsChanged, &loop, [&]() {
        if (!controller.personItemsBusy())
            loop.quit();
    });

    controller.loadPersonItems(QStringLiteral("person-1"));
    timeout.start(timeoutMs);
    if (controller.personItemsBusy())
        loop.exec();
    return !controller.personItemsBusy();
}

} // namespace

SPOOL_TEST_MAIN("content-model-controller")
{
    QCoreApplication app(argc, argv);

    TestCatalog catalog;
    LibraryPrefetchController prefetch(&catalog);
    ContentModelController controller(&catalog, &prefetch);
    MovieItem displayedDetail;
    QObject::connect(&controller, &ContentModelController::detailItemChanged, &controller,
        [&] { displayedDetail = controller.detailItem(); });
    {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&controller, &ContentModelController::detailItemChanged, &loop, [&] {
            if (displayedDetail.id == QStringLiteral("movie-1"))
                loop.quit();
        });
        controller.loadItemDetail(QStringLiteral("movie-1"));
        timeout.start(1000);
        loop.exec();
    }
    require(displayedDetail.id == QStringLiteral("movie-1"), "item details did not finish loading");
    controller.updateResumeTicks(QStringLiteral("movie-1"), 120LL * 10'000'000);
    require(displayedDetail.resumeTicks == 120LL * 10'000'000,
        "first playback did not refresh the displayed detail position");
    controller.updateResumeTicks(QStringLiteral("movie-1"), 240LL * 10'000'000);
    require(
        displayedDetail.resumeTicks == 240LL * 10'000'000, "resumed playback left the displayed detail position stale");
    controller.updateResumeTicks(QStringLiteral("another-movie"), 360LL * 10'000'000);
    require(displayedDetail.resumeTicks == 240LL * 10'000'000,
        "another item's playback changed the displayed detail position");
    Spool::UserItemStateController itemState(nullptr, nullptr, nullptr, &controller, nullptr);
    const MovieItem stoppedItem = displayedDetail;
    itemState.recordPlaybackStopped(stoppedItem, stoppedItem.id, stoppedItem.runtimeTicks, true, {}, 0);
    require(displayedDetail.played && displayedDetail.resumeTicks == 0,
        "completed playback left resumable progress in item details");
    MovieItem staleDetails = stoppedItem;
    staleDetails.resumeTicks = 120LL * 10'000'000;
    staleDetails.played = false;
    auto replayDetails = std::make_shared<QPromise<MovieItem>>();
    replayDetails->start();
    catalog.pendingDetails = replayDetails;
    const MovieItem watchedItem = displayedDetail;
    controller.loadItemDetail(watchedItem.id);
    itemState.recordPlaybackStopped(watchedItem, watchedItem.id, 1LL * 10'000'000, false, {}, 0);
    replayDetails->addResult(staleDetails);
    replayDetails->finish();
    waitUntil([&] { return displayedDetail.id == watchedItem.id; }, "delayed short-replay details did not settle");
    require(displayedDetail.played && displayedDetail.resumeTicks == 0,
        "a short replay and stale details must not unwatch a completed item");
    catalog.pendingDetails.reset();
    controller.updatePlayed(QStringLiteral("movie-1"), false);
    require(!displayedDetail.played && displayedDetail.resumeTicks == 0,
        "marking an item unwatched left stale detail state");
    controller.updateFavorite(QStringLiteral("movie-1"), true);
    require(displayedDetail.favorite, "favorite update left stale detail state");

    SearchController search(&catalog, &prefetch);
    require(waitForSearch(search, 1000), "mixed search did not finish with all result types");
    require(search.movieResults()->rowCount() == 1, "mixed search did not partition its movie result");
    require(search.seriesResults()->rowCount() == 1, "mixed search did not partition its series result");
    require(search.episodeResults()->rowCount() == 1, "mixed search did not partition its episode result");
    require(search.otherResults()->rowCount() == 1, "mixed search discarded its non-video result");
    const MovieItem searchEpisode = search.episodeResults()->get(0);
    require(searchEpisode.seriesPrimaryImageTag == QStringLiteral("series-primary-tag"),
        "episode search result did not retain its series primary artwork tag");
    require(searchEpisode.subtitle() == QStringLiteral("S02:E07"),
        "episode search result did not expose its season and episode label");

    require(waitForPersonRows(controller, 1000), "person credits did not finish loading");
    const QVariantList personRows = controller.personItemRows();
    require(personRows.size() == 3, "person credits did not produce movies, shows, and guest-season rows");
    require(personRows.at(0).toMap().value(QStringLiteral("title")).toString() == QStringLiteral("Movies"),
        "person credits did not prioritize movies");
    auto *personMovies
        = qobject_cast<MovieGridModel *>(personRows.at(0).toMap().value(QStringLiteral("model")).value<QObject *>());
    auto *personShows
        = qobject_cast<MovieGridModel *>(personRows.at(1).toMap().value(QStringLiteral("model")).value<QObject *>());
    auto *guestSeason
        = qobject_cast<MovieGridModel *>(personRows.at(2).toMap().value(QStringLiteral("model")).value<QObject *>());
    require(personMovies && personMovies->rowCount() == 200, "person movie credits were not retained");
    require(
        personShows && personShows->rowCount() == 2, "direct and majority episode credits were not collapsed to shows");
    require(
        personRows.at(2).toMap().value(QStringLiteral("title")).toString() == QStringLiteral("Guest Show · Season 2"),
        "guest episode credits were not grouped by show and season");
    require(guestSeason && guestSeason->rowCount() == 2, "guest season did not retain its matching episodes");

    PagedMovieItems playlistPage;
    QString browseError;
    require(waitForBrowsePage(catalog,
                BrowseDescriptor::playlist(QStringLiteral("playlist-1"), QStringLiteral("Ordered Playlist")), {},
                playlistPage, browseError, 1000),
        "playlist browse page was not fetched");
    require(playlistPage.items.size() == 1, "playlist browse response was not exposed as one item");

    MovieGridModel playlistModel;
    playlistModel.setMovies(playlistPage.items);
    const MovieItem playlistRow = playlistModel.get(0);
    require(playlistRow.id == QStringLiteral("playlist-movie-1"),
        "playlist item id was not populated from the API response");
    require(playlistRow.playlistItemId == QStringLiteral("playlist-item-1"),
        "playlist item snapshot did not preserve PlaylistItemId");

    PagedMovieItems collectionsPage;
    browseError.clear();
    require(
        waitForBrowsePage(catalog,
            BrowseDescriptor::library(QStringLiteral("movies-id"), QStringLiteral("movies"), QStringLiteral("Films")),
            QVariantMap { { QStringLiteral("includeItemTypes"), QStringList { QStringLiteral("BoxSet") } } },
            collectionsPage, browseError, 1000),
        "movie collection-filter browse page was not fetched");
    require(collectionsPage.items.size() == 1 && collectionsPage.items.front().itemType == QStringLiteral("BoxSet"),
        "movie collection-filter browse did not expose BoxSet rows");

    controller.loadDetailRows(
        QStringLiteral("episode-1"), QStringLiteral("Episode"), QStringLiteral("series-1"), QStringLiteral("season-1"));

    require(waitForDetailRowsIdle(controller, 1000), "episode detail rows did not finish loading");

    MovieGridModel *episodes = controller.detailSeasons();
    require(episodes->rowCount() == 1, "episode detail rows did not expose fetched episodes");

    const MovieItem row = episodes->get(0);
    require(row.id == QStringLiteral("episode-row"), "episode detail row id was not populated from the API response");
    require(row.itemType == QStringLiteral("Episode"), "episode detail row type was not preserved");
    require(row.seriesId == QStringLiteral("series-1"), "episode detail row series context was not preserved");
    require(row.seasonId == QStringLiteral("season-1"), "episode detail row season context was not preserved");
    require(episodes->data(episodes->index(0, 0), MovieGridModel::DisplaySubtitleRole).toString()
            == QStringLiteral("S02:E07 · The Loaded Episode"),
        "episode detail row did not use the episode display metadata");
    require(controller.detailSeasonOptions()->rowCount() == 1,
        "episode detail rows did not expose season selector options");
    require(controller.detailSeasonOptions()->get(0).id == QStringLiteral("season-1"),
        "season selector option did not preserve its season id");

    std::vector<MovieItem> seasonEpisodes(6);
    for (int index = 0; index < static_cast<int>(seasonEpisodes.size()); ++index) {
        auto& episode = seasonEpisodes[static_cast<size_t>(index)];
        episode.id = QStringLiteral("abcd1234:episode-%1").arg(index);
        episode.itemType = QStringLiteral("Episode");
        episode.played = index < 3;
    }
    seasonEpisodes[4].resumeTicks = 60LL * 10'000'000;
    catalog.episodeRows = seasonEpisodes;
    const auto loadEpisodeContext = [&](const QString& id, const QString& type) {
        controller.loadDetailRows(id, type, QStringLiteral("series-1"), QStringLiteral("season-1"));
        require(waitForDetailRowsIdle(controller, 1000), "episode context did not settle");
    };
    loadEpisodeContext(QStringLiteral("season-1"), QStringLiteral("Season"));
    require(controller.detailContextInitialIndex() == 4,
        "season details should begin at resumable progress before an earlier unwatched gap");
    loadEpisodeContext(seasonEpisodes[1].id, QStringLiteral("Episode"));
    require(controller.detailContextInitialIndex() == 1,
        "episode details should begin at the current opaque episode id, even if it was watched");
    seasonEpisodes[4].resumeTicks = 0;
    catalog.episodeRows = seasonEpisodes;
    loadEpisodeContext(QStringLiteral("season-1"), QStringLiteral("Season"));
    require(controller.detailContextInitialIndex() == 3,
        "season details should begin at the playable episode after the last watched episode");
    loadEpisodeContext(QStringLiteral("missing"), QStringLiteral("Episode"));
    require(controller.detailContextInitialIndex() == 3,
        "an unavailable current episode should fall back to viewing progress");
    for (auto& episode : seasonEpisodes)
        episode.played = true;
    catalog.episodeRows = seasonEpisodes;
    loadEpisodeContext(QStringLiteral("season-1"), QStringLiteral("Season"));
    require(controller.detailContextInitialIndex() == 5,
        "a completed season should begin at its last watched episode rather than wrap to the beginning");
    controller.reset();
    require(controller.detailContextInitialIndex() == 0 && controller.detailSeasons()->rowCount() == 0,
        "reset should discard both the old episode selection and its rows");
    catalog.episodeRows = std::vector<MovieItem> {};
    loadEpisodeContext(QStringLiteral("season-1"), QStringLiteral("Season"));
    require(controller.detailContextInitialIndex() == 0, "an empty season must not retain an out-of-range index");
    catalog.episodeRows.reset();

    controller.loadDetailRows(QStringLiteral("boxset-1"), QStringLiteral("BoxSet"), QString(), QString());

    require(waitForDetailRowsIdle(controller, 1000), "box set detail rows did not finish loading");

    MovieGridModel *boxSetChildren = controller.detailSeasons();
    require(boxSetChildren->rowCount() == 1, "box set detail rows did not expose collection children");

    const MovieItem boxSetRow = boxSetChildren->get(0);
    require(boxSetRow.id == QStringLiteral("boxset-child-1"),
        "box set child id was not populated from the browse response");
    require(boxSetRow.itemType == QStringLiteral("Movie"), "box set child type was not preserved");

    HomeModelController home(nullptr, &catalog, &prefetch);
    const std::vector<LibraryItem> homeLibraries {
        makeLibrary(QStringLiteral("shows-id"), QStringLiteral("Shows"), QStringLiteral("tvshows")),
        makeLibrary(QStringLiteral("single-show-id"), QStringLiteral("Single Show"), QStringLiteral("tvshows")),
        makeLibrary(QStringLiteral("photos-id"), QStringLiteral("Photos"), QStringLiteral("photos")),
    };
    home.refresh(homeLibraries);
    require(waitForHomeRows(home, 1000), "home latest rows did not finish loading");

    const QVariantList latestRows = home.latestLibraryRows();
    require(latestRows.size() == 3, "home did not expose one latest row for each supported library");
    auto *showItems
        = qobject_cast<MovieGridModel *>(latestRows.at(0).toMap().value(QStringLiteral("model")).value<QObject *>());
    auto *singleShowItems
        = qobject_cast<MovieGridModel *>(latestRows.at(1).toMap().value(QStringLiteral("model")).value<QObject *>());
    auto *photoItems
        = qobject_cast<MovieGridModel *>(latestRows.at(2).toMap().value(QStringLiteral("model")).value<QObject *>());
    require(showItems && showItems->rowCount() == 1, "home did not group latest episodes from one season");
    require(singleShowItems && singleShowItems->rowCount() == 1
            && singleShowItems->get(0).title == QStringLiteral("Series One"),
        "a single latest episode did not use its series title");
    require(photoItems && photoItems->rowCount() == 1 && photoItems->get(0).itemType == QStringLiteral("Photo"),
        "home did not expose an arbitrary-library latest item");
    require(
        showItems->get(0).id == QStringLiteral("series-1") && showItems->get(0).itemType == QStringLiteral("Series"),
        "grouped latest episodes did not navigate as their series");
    require(showItems->get(0).posterTag == QStringLiteral("series-primary-tag"),
        "grouped latest episodes did not use series primary artwork");
    require(!showItems->get(0).isPlayable(),
        "grouped latest episodes still exposed the representative episode as playable");
    require(showItems->get(0).title == QStringLiteral("Series One"),
        "grouped latest episodes did not use the series title");
    require(showItems->get(0).episodeLabel == QStringLiteral("S02 · E01-E145"),
        "grouped latest episodes did not expose the contiguous episode range");
    require(showItems->get(0).subtitle() == QStringLiteral("S02 · E01-E145"),
        "grouped latest show did not display its episode range");
    require(singleShowItems->get(0).id == QStringLiteral("series-1")
            && singleShowItems->get(0).itemType == QStringLiteral("Series") && !singleShowItems->get(0).isPlayable(),
        "a single latest episode did not navigate as its non-playable series");

    int latestStructureChanges = 0;
    QObject::connect(&home, &HomeModelController::latestLibraryRowsChanged,
        [&latestStructureChanges]() { ++latestStructureChanges; });
    MovieItem updatedShow = showItems->get(0);
    updatedShow.title = QStringLiteral("Updated episode");
    const QJsonObject updatedPayload {
        { QStringLiteral("latestRows"),
            QJsonArray {
                QJsonObject {
                    { QStringLiteral("order"), 0 },
                    { QStringLiteral("library"), Spool::metaToJson(homeLibraries[0]) },
                    { QStringLiteral("items"), QJsonArray { Spool::metaToJson(updatedShow) } },
                },
                QJsonObject {
                    { QStringLiteral("order"), 1 },
                    { QStringLiteral("library"), Spool::metaToJson(homeLibraries[1]) },
                    { QStringLiteral("items"), QJsonArray { Spool::metaToJson(singleShowItems->get(0)) } },
                },
                QJsonObject {
                    { QStringLiteral("order"), 2 },
                    { QStringLiteral("library"), Spool::metaToJson(homeLibraries[2]) },
                    { QStringLiteral("items"), QJsonArray { Spool::metaToJson(photoItems->get(0)) } },
                },
            } },
    };
    require(home.applyCachedPayload(updatedPayload), "home rejected an updated snapshot");
    const QVariantList updatedRows = home.latestLibraryRows();
    require(updatedRows.at(0).toMap().value(QStringLiteral("model")).value<QObject *>() == showItems,
        "home replaced a stable latest-row model");
    require(
        showItems->get(0).title == QStringLiteral("Updated episode"), "home did not update a stable latest-row model");
    require(latestStructureChanges == 0, "home emitted a row-structure change for content-only updates");

    TestCatalog changingCatalog;
    LibraryPrefetchController changingPrefetch(&changingCatalog);
    HomeModelController changingHome(nullptr, &changingCatalog, &changingPrefetch);
    const LibraryItem removedLibrary
        = makeLibrary(QStringLiteral("removed:movies"), QStringLiteral("Removed"), QStringLiteral("movies"));
    const LibraryItem retainedLibrary
        = makeLibrary(QStringLiteral("retained:movies"), QStringLiteral("Retained"), QStringLiteral("movies"));
    const auto makeHomeItem = [](const QString& id, const QString& title) {
        MovieItem item;
        item.id = id;
        item.title = title;
        item.itemType = QStringLiteral("Movie");
        return item;
    };
    const MovieItem removedItem = makeHomeItem(QStringLiteral("removed:film"), QStringLiteral("Removed film"));
    MovieItem retainedItem = makeHomeItem(QStringLiteral("retained:film"), QStringLiteral("Retained film"));
    changingCatalog.scopeKey = QStringLiteral("removed+retained");
    changingCatalog.resumeRows = { removedItem, retainedItem };
    changingCatalog.nextUpRows = { removedItem, retainedItem };
    changingCatalog.latestRows.insert(removedLibrary.id, { removedItem });
    changingCatalog.latestRows.insert(retainedLibrary.id, { retainedItem });
    changingHome.refresh({ removedLibrary, retainedLibrary });
    waitUntil([&] { return !changingHome.loading(); }, "initial multi-account homepage did not settle");
    require(changingHome.resumeItems()->get(0).id == removedItem.id
            && changingHome.nextUpItems()->get(0).id == removedItem.id && changingHome.latestLibraryRows().size() == 2,
        "initial homepage must expose both accounts before removing one");
    const auto retainAccount = [](const QString& id) { return id.startsWith(QStringLiteral("retained:")); };
    changingCatalog.scopeKey = QStringLiteral("retained");
    changingHome.invalidate(retainAccount);
    require(changingHome.resumeItems()->rowCount() == 1 && changingHome.resumeItems()->get(0).id == retainedItem.id
            && changingHome.nextUpItems()->rowCount() == 1 && changingHome.nextUpItems()->get(0).id == retainedItem.id
            && changingHome.latestLibraryRows().size() == 1
            && changingHome.latestLibraryRows().first().toMap().value(QStringLiteral("libraryId")).toString()
                == retainedLibrary.id,
        "account removal must immediately discard its home rows while preserving the remaining account");
    retainedItem.title = QStringLiteral("Refreshed retained film");
    changingCatalog.resumeRows = { retainedItem };
    changingCatalog.nextUpRows = { retainedItem };
    changingCatalog.latestRows.insert(retainedLibrary.id, { retainedItem });
    changingHome.refresh({ retainedLibrary });
    waitUntil([&] { return !changingHome.loading(); }, "remaining account homepage did not refresh");
    const auto retainedLatestModel = [&] {
        return qobject_cast<MovieGridModel *>(
            changingHome.latestLibraryRows().first().toMap().value(QStringLiteral("model")).value<QObject *>());
    };
    require(changingHome.resumeItems()->get(0).title == retainedItem.title
            && changingHome.nextUpItems()->get(0).title == retainedItem.title
            && retainedLatestModel()->get(0).title == retainedItem.title,
        "account removal must refresh remaining content even after the previous homepage finished loading");

    changingHome.invalidate();
    auto staleResume = std::make_shared<QPromise<std::vector<MovieItem>>>();
    staleResume->start();
    changingCatalog.pendingResume = staleResume;
    changingCatalog.scopeKey = QStringLiteral("removed+retained");
    changingHome.refresh({ removedLibrary, retainedLibrary });
    require(changingHome.loading(), "controlled old-account resume request must remain in flight");
    changingCatalog.scopeKey = QStringLiteral("retained");
    changingHome.invalidate(retainAccount);
    changingCatalog.pendingResume.reset();
    retainedItem.title = QStringLiteral("Newest retained film");
    changingCatalog.resumeRows = { retainedItem };
    changingCatalog.nextUpRows = { retainedItem };
    changingCatalog.latestRows.insert(retainedLibrary.id, { retainedItem });
    changingHome.refresh({ retainedLibrary });
    waitUntil([&] { return !changingHome.loading(); }, "replacement homepage did not settle");
    staleResume->addResult(std::vector<MovieItem> { removedItem });
    staleResume->finish();
    waitUntil([&] { return changingCatalog.completedHomeRequests == 1; }, "stale resume response did not complete");
    require(!changingHome.loading() && changingHome.resumeItems()->get(0).title == retainedItem.title
            && changingHome.nextUpItems()->get(0).title == retainedItem.title
            && changingHome.latestLibraryRows().size() == 1
            && retainedLatestModel()->get(0).title == retainedItem.title,
        "an old-account resume response must not overwrite the replacement homepage");

    changingHome.invalidate();
    auto staleLatest = std::make_shared<QPromise<std::vector<MovieItem>>>();
    staleLatest->start();
    changingCatalog.pendingLatest.insert(removedLibrary.id, staleLatest);
    changingCatalog.scopeKey = QStringLiteral("removed+retained");
    changingHome.refresh({ removedLibrary, retainedLibrary });
    require(changingHome.loading(), "controlled old-account latest request must remain in flight");
    changingCatalog.scopeKey = QStringLiteral("retained");
    changingHome.invalidate(retainAccount);
    changingCatalog.pendingLatest.clear();
    changingHome.refresh({ retainedLibrary });
    waitUntil([&] { return !changingHome.loading(); }, "replacement latest rows did not settle");
    staleLatest->addResult(std::vector<MovieItem> { removedItem });
    staleLatest->finish();
    waitUntil([&] { return changingCatalog.completedHomeRequests == 2; }, "stale latest response did not complete");
    require(changingHome.latestLibraryRows().size() == 1 && retainedLatestModel()->get(0).id == retainedItem.id
            && retainedLatestModel()->get(0).title == retainedItem.title,
        "a delayed latest response must not resurrect a removed account's library");

    TestCatalog playbackCatalog;
    LibraryPrefetchController playbackPrefetch(&playbackCatalog);
    HomeModelController playbackHome(nullptr, &playbackCatalog, &playbackPrefetch);
    ContentModelController playbackContent(&playbackCatalog, &playbackPrefetch);
    Spool::BrowseSessionController playbackBrowse(&playbackPrefetch);
    Spool::UserItemStateController playbackState(nullptr, &playbackBrowse, &playbackHome, &playbackContent, nullptr);
    MovieItem completedEpisode;
    completedEpisode.id = QStringLiteral("account01:episode-1");
    completedEpisode.seriesId = QStringLiteral("account01:series-1");
    completedEpisode.itemType = QStringLiteral("Episode");
    completedEpisode.resumeTicks = 100'000'000;
    completedEpisode.runtimeTicks = 20'000'000'000;
    MovieItem successor = completedEpisode;
    successor.id = QStringLiteral("account01:episode-2");
    successor.resumeTicks = 0;
    playbackCatalog.resumeRows = { completedEpisode };
    playbackCatalog.nextUpRows = { completedEpisode };
    playbackHome.refresh(homeLibraries);
    waitUntil([&] { return !playbackHome.loading(); }, "initial playback home rows did not settle");
    playbackHome.invalidate();
    auto beforeCompletion = std::make_shared<QPromise<std::vector<MovieItem>>>();
    beforeCompletion->start();
    playbackCatalog.pendingResume = beforeCompletion;
    playbackHome.refresh(homeLibraries);
    playbackState.recordPlaybackStopped(
        completedEpisode, completedEpisode.id, completedEpisode.runtimeTicks, true, successor, 0);
    require(playbackHome.nextUpItems()->count() == 1 && playbackHome.nextUpItems()->get(0).id == successor.id
            && playbackHome.resumeItems()->count() == 0,
        "completion must immediately replace the episode with its successor and clear Continue Watching");
    beforeCompletion->addResult(std::vector<MovieItem> { completedEpisode });
    beforeCompletion->finish();
    waitUntil([&] { return !playbackHome.loading(); }, "pre-completion home request did not settle");
    require(playbackHome.nextUpItems()->count() == 1 && playbackHome.nextUpItems()->get(0).id == successor.id
            && playbackHome.resumeItems()->count() == 0,
        "a home response started before completion must preserve the successor, not resurrect the completed episode");

    require(playbackHome.nextUpItems()->get(0).id == successor.id,
        "the known successor must be visible before the server refresh");
    playbackCatalog.pendingResume.reset();
    playbackHome.refreshPlaybackRows();
    require(playbackHome.nextUpItems()->get(0).id == successor.id && playbackHome.resumeItems()->count() == 0,
        "an eventually consistent response must preserve the optimistic successor, not the completed episode");
    successor.title = QStringLiteral("Server refreshed successor");
    playbackCatalog.resumeRows = {};
    playbackCatalog.nextUpRows = { successor };
    playbackHome.refreshPlaybackRows();
    require(playbackHome.nextUpItems()->get(0).title == successor.title,
        "an already-loaded homepage must accept refreshed Next Up data from the server");

    playbackHome.invalidate();
    auto olderHome = std::make_shared<QPromise<std::vector<MovieItem>>>();
    olderHome->start();
    playbackCatalog.pendingResume = olderHome;
    playbackCatalog.nextUpRows = { completedEpisode };
    playbackHome.refresh(homeLibraries);
    playbackHome.advanceNextUp(completedEpisode, successor);
    playbackHome.updatePlayed(completedEpisode.id, true);
    playbackCatalog.pendingResume.reset();
    playbackCatalog.nextUpRows = { successor };
    playbackHome.refreshPlaybackRows();
    olderHome->addResult(std::vector<MovieItem> { completedEpisode });
    olderHome->finish();
    waitUntil([&] { return !playbackHome.loading(); }, "older full home refresh did not settle");
    require(playbackHome.nextUpItems()->get(0).id == successor.id,
        "an older full refresh cannot erase a successor already confirmed by the newer playback refresh");

    auto beforePartialStop = std::make_shared<QPromise<std::vector<MovieItem>>>();
    beforePartialStop->start();
    playbackCatalog.pendingResume = beforePartialStop;
    playbackHome.refreshPlaybackRows();
    playbackState.recordPlaybackStopped(successor, successor.id, 80'000'000, false, {}, 0);
    beforePartialStop->addResult(std::vector<MovieItem> {});
    beforePartialStop->finish();
    waitUntil([&] { return playbackCatalog.completedHomeRequests == 3; }, "old playback refresh did not settle");
    require(playbackHome.resumeItems()->get(0).id == successor.id
            && playbackHome.resumeItems()->get(0).resumeTicks == 80'000'000,
        "a refresh predating a partial stop cannot erase the newly recorded Continue Watching item");
    auto emptyAfterStop = std::make_shared<QPromise<std::vector<MovieItem>>>();
    emptyAfterStop->start();
    playbackCatalog.pendingResume = emptyAfterStop;
    playbackHome.refreshPlaybackRows();
    emptyAfterStop->addResult(std::vector<MovieItem> {});
    emptyAfterStop->finish();
    waitUntil([&] { return playbackCatalog.completedHomeRequests == 4; },
        "post-stop stale empty resume response did not settle");
    require(playbackHome.resumeItems()->count() == 1 && playbackHome.resumeItems()->get(0).id == successor.id
            && playbackHome.resumeItems()->get(0).resumeTicks == 80'000'000,
        "a new refresh with an empty stale server list must retain authoritative local partial progress");
    playbackState.recordPlaybackStopped(successor, successor.id, successor.runtimeTicks, true, {}, 0);
    require(playbackHome.resumeItems()->count() == 0, "completion must remove the retained local resume row");
    MovieItem staleResumedSuccessor = successor;
    staleResumedSuccessor.resumeTicks = 80'000'000;
    auto staleAfterCompletion = std::make_shared<QPromise<std::vector<MovieItem>>>();
    staleAfterCompletion->start();
    playbackCatalog.pendingResume = staleAfterCompletion;
    playbackHome.refreshPlaybackRows();
    staleAfterCompletion->addResult(std::vector<MovieItem> { staleResumedSuccessor });
    staleAfterCompletion->finish();
    waitUntil([&] { return playbackCatalog.completedHomeRequests == 5; },
        "post-completion stale resume response did not settle");
    require(playbackHome.resumeItems()->count() == 0,
        "an explicit completion must prevent retained local rows or stale server progress from resurrecting the "
        "episode");

    const LibraryItem progressLibrary
        = makeLibrary(QStringLiteral("progress-library"), QStringLiteral("Progress library"), QStringLiteral("movies"));
    const QString progressCacheKey = Spool::libraryCacheKey(progressLibrary);
    auto staleLatestProgress = std::make_shared<QPromise<std::vector<MovieItem>>>();
    staleLatestProgress->start();
    playbackCatalog.pendingResume.reset();
    playbackCatalog.pendingLatest.insert(progressLibrary.id, staleLatestProgress);
    playbackHome.invalidate();
    playbackHome.refresh({ progressLibrary });
    PagedMovieItems cachedProgress;
    cachedProgress.items = { successor };
    cachedProgress.totalRecordCount = 1;
    playbackPrefetch.storePage(progressCacheKey, cachedProgress);
    playbackState.recordPlaybackStopped(successor, successor.id, 140'000'000, false, {}, 0);
    staleLatestProgress->addResult(std::vector<MovieItem> { successor });
    staleLatestProgress->finish();
    waitUntil([&] { return !playbackHome.loading(); }, "delayed latest progress rows did not settle");
    require(playbackHome.latestLibraryRows().size() == 1, "stale latest response did not expose its real row");
    auto *progressRows = qobject_cast<MovieGridModel *>(
        playbackHome.latestLibraryRows().front().toMap().value(QStringLiteral("model")).value<QObject *>());
    require(progressRows && progressRows->get(0).resumeTicks == 140'000'000,
        "a delayed latest row must not revert the newer stopped position");
    require(playbackBrowse.applyCachedPage(progressCacheKey) == 1
            && playbackBrowse.items()->get(0).resumeTicks == 140'000'000,
        "hydrating cached browse rows must use the newer stopped position");
    playbackState.applyPlayed(successor.id, true);
    playbackState.applyPlayed(successor.id, false);
    require(playbackBrowse.applyCachedPage(progressCacheKey) == 1 && !playbackBrowse.items()->get(0).played
            && playbackBrowse.items()->get(0).resumeTicks == 0,
        "marking an episode unwatched must clear cached progress without marking it played");
    cachedProgress.items.front().resumeTicks = 160'000'000;
    playbackPrefetch.storePage(progressCacheKey, cachedProgress);
    playbackContent.reset();
    require(playbackBrowse.applyCachedPage(progressCacheKey) == 1
            && playbackBrowse.items()->get(0).resumeTicks == 160'000'000,
        "session reset must discard old local precedence and accept the new server state");

    return EXIT_SUCCESS;
}
