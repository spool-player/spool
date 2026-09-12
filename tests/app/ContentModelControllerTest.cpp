#include "app/ContentModelController.h"
#include "api/JellyfinApiFacade.h"
#include "app/HomeModelController.h"
#include "app/LibraryPrefetchController.h"
#include "app/SearchController.h"
#include "common/AsyncTask.h"
#include "common/MetaJson.h"
#include "common/TlsTrust.h"

#include "TestMain.h"

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <utility>

using JellyfinNative::AuthSession;
using JellyfinNative::BrowseDescriptor;
using JellyfinNative::ContentModelController;
using JellyfinNative::HomeModelController;
using JellyfinNative::JellyfinApiFacade;
using JellyfinNative::LibraryItem;
using JellyfinNative::LibraryPrefetchController;
using JellyfinNative::MovieGridModel;
using JellyfinNative::MovieItem;
using JellyfinNative::PagedMovieItems;
using JellyfinNative::SearchController;
using JellyfinNative::TlsTrustController;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

QByteArray jsonBytes(const QJsonObject& object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonObject episodeObject(int episodeNumber = 7)
{
    return {
        { QStringLiteral("Id"),
            episodeNumber == 7 ? QStringLiteral("episode-row") : QStringLiteral("episode-%1").arg(episodeNumber) },
        { QStringLiteral("Name"),
            episodeNumber == 7 ? QStringLiteral("The Loaded Episode")
                               : QStringLiteral("Episode %1").arg(episodeNumber) },
        { QStringLiteral("Type"), QStringLiteral("Episode") },
        { QStringLiteral("SeriesId"), QStringLiteral("series-1") },
        { QStringLiteral("SeasonId"), QStringLiteral("season-1") },
        { QStringLiteral("SeriesName"), QStringLiteral("Series One") },
        { QStringLiteral("SeriesPrimaryImageTag"), QStringLiteral("series-primary-tag") },
        { QStringLiteral("ParentIndexNumber"), 2 },
        { QStringLiteral("IndexNumber"), episodeNumber },
    };
}
QJsonObject movieObject()
{
    return {
        { QStringLiteral("Id"), QStringLiteral("movie-1") },
        { QStringLiteral("Name"), QStringLiteral("Movie One") },
        { QStringLiteral("Type"), QStringLiteral("Movie") },
    };
}

QJsonObject seriesObject()
{
    return {
        { QStringLiteral("Id"), QStringLiteral("series-1") },
        { QStringLiteral("Name"), QStringLiteral("Series One") },
        { QStringLiteral("Type"), QStringLiteral("Series") },
    };
}

QJsonObject personEpisodeObject(
    const QString& id, const QString& seriesId, const QString& seriesName, int seasonNumber, int episodeNumber)
{
    return {
        { QStringLiteral("Id"), id },
        { QStringLiteral("Name"), QStringLiteral("Episode %1").arg(episodeNumber) },
        { QStringLiteral("Type"), QStringLiteral("Episode") },
        { QStringLiteral("SeriesId"), seriesId },
        { QStringLiteral("SeasonId"), QStringLiteral("%1-season-%2").arg(seriesId).arg(seasonNumber) },
        { QStringLiteral("SeriesName"), seriesName },
        { QStringLiteral("ParentIndexNumber"), seasonNumber },
        { QStringLiteral("IndexNumber"), episodeNumber },
    };
}

QJsonObject personSeriesObject(const QString& id, const QString& name, int episodeCount)
{
    return {
        { QStringLiteral("Id"), id },
        { QStringLiteral("Name"), name },
        { QStringLiteral("Type"), QStringLiteral("Series") },
        { QStringLiteral("RecursiveItemCount"), episodeCount },
    };
}

QJsonObject seasonObject()
{
    return {
        { QStringLiteral("Id"), QStringLiteral("season-1") },
        { QStringLiteral("Name"), QStringLiteral("Season 2") },
        { QStringLiteral("Type"), QStringLiteral("Season") },
        { QStringLiteral("SeriesId"), QStringLiteral("series-1") },
        { QStringLiteral("SeriesName"), QStringLiteral("Series One") },
        { QStringLiteral("IndexNumber"), 2 },
    };
}

QJsonObject photoObject()
{
    return {
        { QStringLiteral("Id"), QStringLiteral("photo-1") },
        { QStringLiteral("Name"), QStringLiteral("Photo One") },
        { QStringLiteral("Type"), QStringLiteral("Photo") },
    };
}

LibraryItem library(const QString& id, const QString& name, const QString& collectionType)
{
    LibraryItem item;
    item.id = id;
    item.name = name;
    item.collectionType = collectionType;
    return item;
}

QJsonObject playlistMovieObject()
{
    return {
        { QStringLiteral("Id"), QStringLiteral("playlist-movie-1") },
        { QStringLiteral("Name"), QStringLiteral("Playlist Movie") },
        { QStringLiteral("Type"), QStringLiteral("Movie") },
        { QStringLiteral("PlaylistItemId"), QStringLiteral("playlist-item-1") },
    };
}

QJsonObject collectionObject()
{
    return {
        { QStringLiteral("Id"), QStringLiteral("boxset-1") },
        { QStringLiteral("Name"), QStringLiteral("A Collection") },
        { QStringLiteral("Type"), QStringLiteral("BoxSet") },
    };
}

QJsonObject boxSetChildObject()
{
    return {
        { QStringLiteral("Id"), QStringLiteral("boxset-child-1") },
        { QStringLiteral("Name"), QStringLiteral("Collection Child") },
        { QStringLiteral("Type"), QStringLiteral("Movie") },
        { QStringLiteral("ProductionYear"), 1999 },
    };
}

class MemoryReply final : public QNetworkReply {
public:
    MemoryReply(const QNetworkRequest& request, QNetworkAccessManager::Operation operation, QByteArray payload,
        int statusCode, QObject *parent)
        : QNetworkReply(parent)
        , m_payload(std::move(payload))
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(operation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, statusCode);
        setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this]() {
            emit readyRead();
            emit finished();
        });
    }

    void abort() override { }

    qint64 bytesAvailable() const override
    {
        return static_cast<qint64>(m_payload.size() - m_offset) + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        if (m_offset >= m_payload.size() || maxSize <= 0)
            return -1;

        const qint64 length = std::min<qint64>(maxSize, m_payload.size() - m_offset);
        std::memcpy(data, m_payload.constData() + m_offset, static_cast<size_t>(length));
        m_offset += length;
        return length;
    }

private:
    QByteArray m_payload;
    qsizetype m_offset = 0;
};

class FakeNetworkAccessManager final : public QNetworkAccessManager {
public:
    QVector<QUrl> requestedUrls;
    QVector<int> connectionCacheExpirySeconds;

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest& request, QIODevice *outgoingData) override
    {
        Q_UNUSED(outgoingData);
        requestedUrls.push_back(request.url());
        connectionCacheExpirySeconds.push_back(
            request.attribute(QNetworkRequest::ConnectionCacheExpiryTimeoutSecondsAttribute).toInt());

        const QUrl url = request.url();
        const QUrlQuery query(url);
        if (operation == GetOperation && url.path() == QStringLiteral("/Users/user-1/Items/movie-1")) {
            QJsonObject item = movieObject();
            item.insert(QStringLiteral("RunTimeTicks"), 1200LL * 10'000'000);
            return new MemoryReply(request, operation, jsonBytes(item), 200, this);
        }
        if (operation == GetOperation && url.path() == QStringLiteral("/Items")
            && query.queryItemValue(QStringLiteral("personIds")) == QStringLiteral("person-1")) {
            const int startIndex = query.queryItemValue(QStringLiteral("startIndex")).toInt();
            QJsonArray items;
            if (startIndex == 0) {
                for (int index = 0; index < 200; ++index) {
                    items.push_back(QJsonObject {
                        { QStringLiteral("Id"), QStringLiteral("person-movie-%1").arg(index) },
                        { QStringLiteral("Name"), QStringLiteral("Movie %1").arg(index, 3, 10, QLatin1Char('0')) },
                        { QStringLiteral("Type"), QStringLiteral("Movie") },
                    });
                }
            } else if (startIndex == 200) {
                items = QJsonArray {
                    personSeriesObject(QStringLiteral("series-direct"), QStringLiteral("Direct Show"), 100),
                    personEpisodeObject(QStringLiteral("direct-episode"), QStringLiteral("series-direct"),
                        QStringLiteral("Direct Show"), 1, 1),
                    personEpisodeObject(
                        QStringLiteral("guest-1"), QStringLiteral("series-guest"), QStringLiteral("Guest Show"), 2, 1),
                    personEpisodeObject(
                        QStringLiteral("guest-2"), QStringLiteral("series-guest"), QStringLiteral("Guest Show"), 2, 2),
                    personEpisodeObject(QStringLiteral("majority-1"), QStringLiteral("series-majority"),
                        QStringLiteral("Majority Show"), 1, 1),
                    personEpisodeObject(QStringLiteral("majority-2"), QStringLiteral("series-majority"),
                        QStringLiteral("Majority Show"), 1, 2),
                    personEpisodeObject(QStringLiteral("majority-3"), QStringLiteral("series-majority"),
                        QStringLiteral("Majority Show"), 1, 3),
                };
            }
            return new MemoryReply(request, operation,
                jsonBytes({ { QStringLiteral("Items"), items }, { QStringLiteral("TotalRecordCount"), 207 } }), 200,
                this);
        }

        if (operation == GetOperation && url.path() == QStringLiteral("/Items")
            && query.hasQueryItem(QStringLiteral("ids"))) {
            return new MemoryReply(request, operation,
                jsonBytes({ { QStringLiteral("Items"),
                    QJsonArray {
                        personSeriesObject(QStringLiteral("series-direct"), QStringLiteral("Direct Show"), 100),
                        personSeriesObject(QStringLiteral("series-guest"), QStringLiteral("Guest Show"), 10),
                        personSeriesObject(QStringLiteral("series-majority"), QStringLiteral("Majority Show"), 4),
                    } } }),
                200, this);
        }

        if (operation == GetOperation && url.path() == QStringLiteral("/Items")
            && query.queryItemValue(QStringLiteral("searchTerm")) == QStringLiteral("mixed")) {
            return new MemoryReply(request, operation,
                jsonBytes({ { QStringLiteral("Items"),
                    QJsonArray { movieObject(), seriesObject(), episodeObject(), photoObject() } } }),
                200, this);
        }

        if (operation == GetOperation && url.path() == QStringLiteral("/Users/user-1/Items/Resume"))
            return new MemoryReply(
                request, operation, jsonBytes({ { QStringLiteral("Items"), QJsonArray {} } }), 200, this);

        if (operation == GetOperation && url.path() == QStringLiteral("/Shows/NextUp"))
            return new MemoryReply(
                request, operation, jsonBytes({ { QStringLiteral("Items"), QJsonArray {} } }), 200, this);

        if (operation == GetOperation && url.path() == QStringLiteral("/Users/user-1/Items/Latest")) {
            const QString parentId = query.queryItemValue(QStringLiteral("parentId"));
            QJsonArray items;
            if (parentId == QStringLiteral("shows-id")) {
                const int count = std::min(query.queryItemValue(QStringLiteral("limit")).toInt(), 145);
                for (int episode = 1; episode <= count; ++episode)
                    items.push_back(episodeObject(episode));
            } else if (parentId == QStringLiteral("single-show-id")) {
                items.push_back(episodeObject(3));
            } else if (parentId == QStringLiteral("photos-id")) {
                items.push_back(photoObject());
            }
            return new MemoryReply(request, operation, jsonBytes({ { QStringLiteral("Items"), items } }), 200, this);
        }

        if (operation == GetOperation && url.path() == QStringLiteral("/Shows/series-1/Episodes")
            && query.queryItemValue(QStringLiteral("seasonId")) == QStringLiteral("season-1")) {
            return new MemoryReply(request, operation,
                jsonBytes({ { QStringLiteral("Items"), QJsonArray { episodeObject() } } }), 200, this);
        }

        if (operation == GetOperation && url.path() == QStringLiteral("/Shows/series-1/Episodes")) {
            return new MemoryReply(
                request, operation, jsonBytes({ { QStringLiteral("Items"), QJsonArray {} } }), 200, this);
        }

        if (operation == GetOperation && url.path() == QStringLiteral("/Shows/series-1/Seasons")) {
            return new MemoryReply(request, operation,
                jsonBytes({ { QStringLiteral("Items"), QJsonArray { seasonObject() } } }), 200, this);
        }

        if (operation == GetOperation && url.path() == QStringLiteral("/Playlists/playlist-1/Items")) {
            return new MemoryReply(request, operation,
                jsonBytes({ { QStringLiteral("Items"), QJsonArray { playlistMovieObject() } },
                    { QStringLiteral("TotalRecordCount"), 1 } }),
                200, this);
        }

        if (operation == GetOperation && url.path() == QStringLiteral("/Items")
            && query.queryItemValue(QStringLiteral("parentId")) == QStringLiteral("movies-id")
            && query.queryItemValue(QStringLiteral("includeItemTypes")) == QStringLiteral("BoxSet")
            && !query.hasQueryItem(QStringLiteral("mediaTypes"))) {
            return new MemoryReply(request, operation,
                jsonBytes({ { QStringLiteral("Items"), QJsonArray { collectionObject() } },
                    { QStringLiteral("TotalRecordCount"), 1 } }),
                200, this);
        }

        if (operation == GetOperation && url.path() == QStringLiteral("/Items")
            && query.queryItemValue(QStringLiteral("parentId")) == QStringLiteral("boxset-1")) {
            return new MemoryReply(request, operation,
                jsonBytes({ { QStringLiteral("Items"), QJsonArray { boxSetChildObject() } },
                    { QStringLiteral("TotalRecordCount"), 1 } }),
                200, this);
        }

        if (operation == GetOperation
            && (url.path() == QStringLiteral("/Items/episode-1/Similar")
                || url.path() == QStringLiteral("/Items/boxset-1/Similar"))) {
            return new MemoryReply(
                request, operation, jsonBytes({ { QStringLiteral("Items"), QJsonArray {} } }), 200, this);
        }

        return new MemoryReply(
            request, operation, jsonBytes({ { QStringLiteral("Items"), QJsonArray {} } }), 404, this);
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

bool waitForBrowsePage(JellyfinApiFacade& api, const BrowseDescriptor& descriptor, const QVariantMap& queryOptions,
    PagedMovieItems& page, QString& error, int timeoutMs)
{
    bool finished = false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    JellyfinNative::Async::runDetached(
        api.fetchBrowsePage(descriptor, 0, 72, queryOptions),
        [&page, &finished, &loop](PagedMovieItems value) {
            page = std::move(value);
            finished = true;
            loop.quit();
        },
        [&error, &finished, &loop](const std::exception_ptr& exception) {
            error = JellyfinNative::exceptionMessage(exception);
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
    return changed;
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

bool personCreditsWerePaged(const QVector<QUrl>& urls)
{
    QSet<int> starts;
    for (const QUrl& url : urls) {
        const QUrlQuery query(url);
        if (url.path() != QStringLiteral("/Items")
            || query.queryItemValue(QStringLiteral("personIds")) != QStringLiteral("person-1"))
            continue;
        require(!query.hasQueryItem(QStringLiteral("mediaTypes")),
            "person credits constrained media types in a way that excludes series");
        starts.insert(query.queryItemValue(QStringLiteral("startIndex")).toInt());
    }
    return starts.contains(0) && starts.contains(200);
}

int searchRequestCount(const QVector<QUrl>& urls)
{
    return static_cast<int>(std::count_if(urls.cbegin(), urls.cend(), [](const QUrl& url) {
        return url.path() == QStringLiteral("/Items")
            && QUrlQuery(url).queryItemValue(QStringLiteral("searchTerm")) == QStringLiteral("mixed");
    }));
}

bool searchRequestAllowsSeries(const QVector<QUrl>& urls)
{
    return std::any_of(urls.cbegin(), urls.cend(), [](const QUrl& url) {
        const QUrlQuery query(url);
        return url.path() == QStringLiteral("/Items")
            && query.queryItemValue(QStringLiteral("searchTerm")) == QStringLiteral("mixed")
            && query.queryItemValue(QStringLiteral("includeItemTypes")).contains(QStringLiteral("Series"))
            && !query.hasQueryItem(QStringLiteral("mediaTypes"));
    });
}

bool searchRequestAllowsMixedLibraries(const QVector<QUrl>& urls)
{
    return std::any_of(urls.cbegin(), urls.cend(), [](const QUrl& url) {
        const QUrlQuery query(url);
        const QString types = query.queryItemValue(QStringLiteral("includeItemTypes"));
        return query.queryItemValue(QStringLiteral("searchTerm")) == QStringLiteral("mixed")
            && types.contains(QStringLiteral("Audio")) && types.contains(QStringLiteral("Photo"))
            && types.contains(QStringLiteral("Person")) && !query.hasQueryItem(QStringLiteral("parentId"));
    });
}

int latestRequestCount(const QVector<QUrl>& urls)
{
    return static_cast<int>(std::count_if(urls.cbegin(), urls.cend(),
        [](const QUrl& url) { return url.path() == QStringLiteral("/Users/user-1/Items/Latest"); }));
}

bool latestRequestsAreUnfiltered(const QVector<QUrl>& urls)
{
    return std::all_of(urls.cbegin(), urls.cend(), [](const QUrl& url) {
        return url.path() != QStringLiteral("/Users/user-1/Items/Latest")
            || !QUrlQuery(url).hasQueryItem(QStringLiteral("includeItemTypes"));
    });
}

bool requestedOrderedPlaylistItems(const QVector<QUrl>& urls)
{
    return std::any_of(urls.cbegin(), urls.cend(), [](const QUrl& url) {
        const QUrlQuery query(url);

        return url.path() == QStringLiteral("/Playlists/playlist-1/Items")
            && !query.hasQueryItem(QStringLiteral("parentId")) && !query.hasQueryItem(QStringLiteral("sortBy"));
    });
}

bool requestedMovieCollections(const QVector<QUrl>& urls)
{
    return std::any_of(urls.cbegin(), urls.cend(), [](const QUrl& url) {
        const QUrlQuery query(url);
        return url.path() == QStringLiteral("/Items")
            && query.queryItemValue(QStringLiteral("parentId")) == QStringLiteral("movies-id")
            && query.queryItemValue(QStringLiteral("includeItemTypes")) == QStringLiteral("BoxSet")
            && query.queryItemValue(QStringLiteral("recursive")) == QStringLiteral("false")
            && !query.hasQueryItem(QStringLiteral("mediaTypes"));
    });
}

bool requestedBoxSetChildren(const QVector<QUrl>& urls)
{
    return std::any_of(urls.cbegin(), urls.cend(), [](const QUrl& url) {
        const QUrlQuery query(url);
        const QStringList types
            = query.queryItemValue(QStringLiteral("includeItemTypes")).split(QLatin1Char(','), Qt::SkipEmptyParts);
        return url.path() == QStringLiteral("/Items")
            && query.queryItemValue(QStringLiteral("parentId")) == QStringLiteral("boxset-1")
            && query.queryItemValue(QStringLiteral("recursive")) == QStringLiteral("false")
            && types.contains(QStringLiteral("Movie")) && types.contains(QStringLiteral("Series"))
            && types.contains(QStringLiteral("Episode"));
    });
}

bool requestedPathWithSeason(const QVector<QUrl>& urls)
{
    return std::any_of(urls.cbegin(), urls.cend(), [](const QUrl& url) {
        const QUrlQuery query(url);
        return url.path() == QStringLiteral("/Shows/series-1/Episodes")
            && query.queryItemValue(QStringLiteral("seasonId")) == QStringLiteral("season-1");
    });
}

} // namespace

JELLYFIN_TEST_MAIN("content-model-controller")
{
    QCoreApplication app(argc, argv);

    FakeNetworkAccessManager network;
    TlsTrustController tlsTrust;
    JellyfinApiFacade api(&network, &tlsTrust);
    api.setServerUrl(QStringLiteral("http://192.168.1.2"));
    api.setSession(AuthSession {
        QStringLiteral("user-1"), QStringLiteral("Tester"), QStringLiteral("token-1"), QStringLiteral("server-1") });
    LibraryPrefetchController prefetch(&api);
    ContentModelController controller(&api, &prefetch);
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

    SearchController search(&api, &prefetch);
    require(waitForSearch(search, 1000), "mixed search did not finish with all result types");
    require(searchRequestCount(network.requestedUrls) == 2,
        "mixed search should issue a broad query and one series-safe query");
    require(searchRequestAllowsSeries(network.requestedUrls),
        "mixed search constrained media types in a way that excludes series containers");
    require(searchRequestAllowsMixedLibraries(network.requestedUrls),
        "mixed search omitted media types used by music, photo, or people libraries");
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
    require(personCreditsWerePaged(network.requestedUrls), "person credits stopped after the first API page");
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
    require(waitForBrowsePage(api,
                BrowseDescriptor::playlist(QStringLiteral("playlist-1"), QStringLiteral("Ordered Playlist")), {},
                playlistPage, browseError, 1000),
        "playlist browse page was not fetched");
    require(requestedOrderedPlaylistItems(network.requestedUrls),
        "playlist browse did not use the ordered playlist items endpoint");
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
        waitForBrowsePage(api,
            BrowseDescriptor::library(QStringLiteral("movies-id"), QStringLiteral("movies"), QStringLiteral("Films")),
            QVariantMap { { QStringLiteral("includeItemTypes"), QStringList { QStringLiteral("BoxSet") } } },
            collectionsPage, browseError, 1000),
        "movie collection-filter browse page was not fetched");
    require(requestedMovieCollections(network.requestedUrls),
        "movie collection filter did not request BoxSet without a video media type");
    require(collectionsPage.items.size() == 1 && collectionsPage.items.front().itemType == QStringLiteral("BoxSet"),
        "movie collection-filter browse did not expose BoxSet rows");

    controller.loadDetailRows(
        QStringLiteral("episode-1"), QStringLiteral("Episode"), QStringLiteral("series-1"), QStringLiteral("season-1"));

    require(waitForDetailRowsIdle(controller, 1000), "episode detail rows did not finish loading");
    require(requestedPathWithSeason(network.requestedUrls),
        "episode detail rows did not request the selected season's episodes");

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

    controller.loadDetailRows(QStringLiteral("boxset-1"), QStringLiteral("BoxSet"), QString(), QString());

    require(waitForDetailRowsIdle(controller, 1000), "box set detail rows did not finish loading");
    require(requestedBoxSetChildren(network.requestedUrls),
        "box set detail rows did not request collection children through browse");

    MovieGridModel *boxSetChildren = controller.detailSeasons();
    require(boxSetChildren->rowCount() == 1, "box set detail rows did not expose collection children");

    const MovieItem boxSetRow = boxSetChildren->get(0);
    require(boxSetRow.id == QStringLiteral("boxset-child-1"),
        "box set child id was not populated from the browse response");
    require(boxSetRow.itemType == QStringLiteral("Movie"), "box set child type was not preserved");

    HomeModelController home(nullptr, &api, &prefetch);
    const std::vector<LibraryItem> homeLibraries {
        library(QStringLiteral("shows-id"), QStringLiteral("Shows"), QStringLiteral("tvshows")),
        library(QStringLiteral("single-show-id"), QStringLiteral("Single Show"), QStringLiteral("tvshows")),
        library(QStringLiteral("photos-id"), QStringLiteral("Photos"), QStringLiteral("photos")),
    };
    home.refresh(homeLibraries);
    require(latestRequestCount(network.requestedUrls) == 3,
        "home latest requests were not all dispatched before the event loop resumed");
    require(latestRequestsAreUnfiltered(network.requestedUrls),
        "home latest requests retained a movie/series/episode-only filter");
    require(waitForHomeRows(home, 1000), "home latest rows did not finish loading");
    require(latestRequestCount(network.requestedUrls) == 4,
        "home did not expand the latest request until grouping exhausted the server results");

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
                    { QStringLiteral("library"), JellyfinNative::metaToJson(homeLibraries[0]) },
                    { QStringLiteral("items"), QJsonArray { JellyfinNative::metaToJson(updatedShow) } },
                },
                QJsonObject {
                    { QStringLiteral("order"), 1 },
                    { QStringLiteral("library"), JellyfinNative::metaToJson(homeLibraries[1]) },
                    { QStringLiteral("items"), QJsonArray { JellyfinNative::metaToJson(singleShowItems->get(0)) } },
                },
                QJsonObject {
                    { QStringLiteral("order"), 2 },
                    { QStringLiteral("library"), JellyfinNative::metaToJson(homeLibraries[2]) },
                    { QStringLiteral("items"), QJsonArray { JellyfinNative::metaToJson(photoItems->get(0)) } },
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

    require(!network.connectionCacheExpirySeconds.isEmpty()
            && std::all_of(network.connectionCacheExpirySeconds.cbegin(), network.connectionCacheExpirySeconds.cend(),
                [](int seconds) { return seconds >= 15 * 60; }),
        "API requests did not preserve idle server connections across navigation pauses");

    return EXIT_SUCCESS;
}
