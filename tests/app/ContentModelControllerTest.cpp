#include "app/ContentModelController.h"
#include "app/HomeModelController.h"
#include "app/LibraryPrefetchController.h"
#include "app/SearchController.h"
#include "common/AsyncTask.h"
#include "common/MetaJson.h"
#include "provider/Catalog.h"
#include "provider/SearchSource.h"

#include "TestMain.h"

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <algorithm>
#include <cstdlib>
#include <iostream>
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

void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

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
    bool signedIn() const override
    {
        return true;
    }

    QString libraryScopeKey() const override
    {
        return QStringLiteral("test-scope");
    }

    QCoro::Task<PagedMovieItems> fetchBrowsePage(
        BrowseDescriptor descriptor, int startIndex = 0, int limit = 72, QVariantMap queryOptions = {}) override
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
        MovieItem item;
        item.id = itemId;
        item.title = QStringLiteral("Movie One");
        item.itemType = QStringLiteral("Movie");
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
        co_return std::vector<MovieItem> {};
    }

    QCoro::Task<std::vector<MovieItem>> fetchNextUpEpisodes(int limit = 24) override
    {
        Q_UNUSED(limit);
        co_return std::vector<MovieItem> {};
    }

    QCoro::Task<std::vector<MovieItem>> fetchLatestItems(QString parentId = {}, int limit = 24) override
    {
        Q_UNUSED(limit);
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
        catalog.fetchBrowsePage(descriptor, 0, 72, queryOptions),
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
    controller.updatePlayed(QStringLiteral("movie-1"), true);
    require(displayedDetail.played && displayedDetail.resumeTicks == 0,
        "completed playback left resumable progress in item details");
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

    return EXIT_SUCCESS;
}
