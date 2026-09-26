#include "ContentModelController.h"

#include "../common/AsyncTask.h"
#include "LibraryPrefetchController.h"

#include <QDebug>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <utility>

namespace Spool {

ContentModelController::ContentModelController(Catalog *catalog, LibraryPrefetchController *prefetch, QObject *parent)
    : QObject(parent)
    , m_api(catalog)
    , m_prefetch(prefetch)
{
}

QVariantMap ContentModelController::detailMediaInfo(const QString& preferredAudioLanguage) const
{
    return formatMediaInfo(m_detailItem, preferredAudioLanguage);
}

QVariantList ContentModelController::personItemRows() const
{
    QVariantList rows;
    rows.reserve(static_cast<qsizetype>(m_personItemSections.size()));
    for (const PersonItemSection& section : m_personItemSections) {
        if (!section.model || section.model->rowCount() <= 0)
            continue;
        rows.push_back(QVariantMap {
            { QStringLiteral("title"), section.title },
            { QStringLiteral("kind"), section.kind },
            { QStringLiteral("model"), QVariant::fromValue(static_cast<QObject *>(section.model.get())) },
        });
    }
    return rows;
}

void ContentModelController::loadDetailRows(
    const QString& itemId, const QString& itemType, const QString& seriesId, const QString& seasonId)
{
    const RequestGeneration::Token generation = m_detailRowsGeneration.next();
    m_detailRowsPending = 0;
    m_detailRowsBusy = false;
    m_detailContextInitialIndex = 0;
    m_detailSeasons.clear();
    m_detailSeasonOptions.clear();
    m_detailSimilarItems.clear();

    if (itemId.isEmpty() || !m_api || !m_api->signedIn()) {
        emit detailRowsChanged();
        return;
    }

    const bool loadSeasons = itemType == QStringLiteral("Series");
    const bool loadEpisodes
        = (itemType == QStringLiteral("Episode") || itemType == QStringLiteral("Season")) && !seriesId.isEmpty();
    const bool loadSeasonOptions = loadEpisodes;
    const bool loadBoxSet = itemType == QStringLiteral("BoxSet");
    const bool loadAlbum = itemType == QStringLiteral("MusicAlbum");
    const bool loadContext = loadSeasons || loadEpisodes || loadBoxSet || loadAlbum;
    m_detailRowsBusy = true;
    m_detailRowsPending = 1 + (loadContext ? 1 : 0) + (loadSeasonOptions ? 1 : 0);
    emit detailRowsChanged();

    qInfo() << "detail rows: loading" << itemType << itemId << "context="
            << (loadSeasons           ? "seasons"
                       : loadEpisodes ? "episodes"
                       : loadBoxSet   ? "boxset"
                       : loadAlbum    ? "album"
                                      : "none");

    if (loadSeasons) {
        Async::runLatest(
            this, m_api->fetchSeasons(itemId), m_detailRowsGeneration, generation,
            [this, generation, itemId](const std::vector<MovieItem>& seasons) {
                qInfo() << "detail rows: seasons loaded" << itemId << seasons.size();
                m_detailSeasons.setMovies(seasons);
                m_prefetch->prefetchPosters(seasons);
                emit detailRowsChanged();
                finishDetailRowLoad(generation);
            },
            [this, generation, itemId](const std::exception_ptr& error) {
                qWarning() << "detail rows: seasons fetch failed" << itemId << exceptionMessage(error);
                finishDetailRowLoad(generation);
            });
    } else if (loadEpisodes) {
        Async::runLatest(
            this, m_api->fetchEpisodes(seriesId, seasonId), m_detailRowsGeneration, generation,
            [this, generation, seriesId, itemId, itemType](const std::vector<MovieItem>& episodes) {
                qInfo() << "detail rows: episodes loaded" << seriesId << episodes.size();
                int initialIndex = episodicPlaybackStartIndex(episodes);
                if (initialIndex < 0) {
                    initialIndex = 0;
                    for (int index = 0; index < static_cast<int>(episodes.size()); ++index) {
                        if (episodes[static_cast<size_t>(index)].played)
                            initialIndex = index;
                    }
                }
                if (itemType == QStringLiteral("Episode")) {
                    const auto current = std::find_if(episodes.cbegin(), episodes.cend(),
                        [&itemId](const MovieItem& episode) { return episode.id == itemId; });
                    if (current != episodes.cend())
                        initialIndex = static_cast<int>(std::distance(episodes.cbegin(), current));
                }
                m_detailContextInitialIndex = initialIndex;
                m_detailSeasons.setMovies(episodes);
                m_prefetch->prefetchPosters(episodes);
                emit detailRowsChanged();
                finishDetailRowLoad(generation);
            },
            [this, generation, seriesId](const std::exception_ptr& error) {
                qWarning() << "detail rows: episodes fetch failed" << seriesId << exceptionMessage(error);
                finishDetailRowLoad(generation);
            });
    } else if (loadBoxSet) {
        Async::runLatest(
            this, m_api->fetchBrowsePage(BrowseDescriptor::boxSet(itemId), 0, 200), m_detailRowsGeneration, generation,
            [this, generation, itemId](const PagedMovieItems& page) {
                qInfo() << "detail rows: box set children loaded" << itemId << page.items.size();
                m_detailSeasons.setMovies(page.items);
                m_prefetch->prefetchPosters(page.items);
                emit detailRowsChanged();
                finishDetailRowLoad(generation);
            },
            [this, generation, itemId](const std::exception_ptr& error) {
                qWarning() << "detail rows: box set children fetch failed" << itemId << exceptionMessage(error);
                finishDetailRowLoad(generation);
            });
    } else if (loadAlbum) {
        Async::runLatest(
            this, m_api->fetchBrowsePage(BrowseDescriptor::folderChildren(itemId), 0, 200), m_detailRowsGeneration,
            generation,
            [this, generation, itemId](const PagedMovieItems& page) {
                qInfo() << "detail rows: album tracks loaded" << itemId << page.items.size();
                m_detailSeasons.setMovies(page.items);
                emit detailRowsChanged();
                finishDetailRowLoad(generation);
            },
            [this, generation, itemId](const std::exception_ptr& error) {
                qWarning() << "detail rows: album tracks fetch failed" << itemId << exceptionMessage(error);
                finishDetailRowLoad(generation);
            });
    }

    if (loadSeasonOptions) {
        Async::runLatest(
            this, m_api->fetchSeasons(seriesId), m_detailRowsGeneration, generation,
            [this, generation, seriesId](const std::vector<MovieItem>& seasons) {
                qInfo() << "detail rows: season options loaded" << seriesId << seasons.size();
                m_detailSeasonOptions.setMovies(seasons);
                m_prefetch->prefetchPosters(seasons);
                emit detailRowsChanged();
                finishDetailRowLoad(generation);
            },
            [this, generation, seriesId](const std::exception_ptr& error) {
                qWarning() << "detail rows: season options fetch failed" << seriesId << exceptionMessage(error);
                finishDetailRowLoad(generation);
            });
    }

    Async::runLatest(
        this, m_api->fetchSimilarItems(itemId), m_detailRowsGeneration, generation,
        [this, generation, itemId](const std::vector<MovieItem>& items) {
            qInfo() << "detail rows: similar loaded" << itemId << items.size();
            m_detailSimilarItems.setMovies(items);
            m_prefetch->prefetchPosters(items);
            emit detailRowsChanged();
            finishDetailRowLoad(generation);
        },
        [this, generation, itemId](const std::exception_ptr& error) {
            qWarning() << "detail rows: similar fetch failed" << itemId << exceptionMessage(error);
            finishDetailRowLoad(generation);
        });
}

void ContentModelController::loadItemDetail(const QString& itemId)
{
    const RequestGeneration::Token generation = m_detailItemGeneration.next();
    m_detailItem = {};

    if (itemId.isEmpty() || !m_api || !m_api->signedIn()) {
        emit detailItemChanged();
        return;
    }

    emit detailItemChanged();

    Async::runLatest(
        this, m_api->fetchItemDetails(itemId), m_detailItemGeneration, generation,
        [this](const MovieItem& item) {
            m_detailItem = item;
            emit detailItemChanged();
        },
        [this](const std::exception_ptr& error) {
            m_detailItem = {};
            emit detailItemChanged();
            emit errorOccurred(exceptionMessage(error));
        });
}

void ContentModelController::loadPersonItems(const QString& personId)
{
    const RequestGeneration::Token generation = m_personItemsGeneration.next();
    clearPersonItems();
    if (personId.isEmpty() || !m_api || !m_api->signedIn()) {
        m_personItemsBusy = false;
        emit personItemsChanged();
        return;
    }

    m_personItemsBusy = true;
    emit personItemsChanged();

    Async::runLatest(
        this, m_api->fetchItemsByPerson(personId), m_personItemsGeneration, generation,
        [this](PersonCredits credits) {
            setPersonCredits(std::move(credits));
            m_personItemsBusy = false;
            emit personItemsChanged();
        },
        [this](const std::exception_ptr& error) {
            clearPersonItems();
            m_personItemsBusy = false;
            emit personItemsChanged();
            emit errorOccurred(exceptionMessage(error));
        });
}

void ContentModelController::setPersonCredits(PersonCredits credits)
{
    clearPersonItems();

    QHash<QString, MovieItem> relatedSeries;
    for (MovieItem& series : credits.relatedSeries) {
        if (!series.id.isEmpty())
            relatedSeries.insert(series.id, std::move(series));
    }

    std::vector<MovieItem> movies;
    QHash<QString, MovieItem> showsById;
    QSet<QString> directShowCredits;
    QHash<QString, std::vector<MovieItem>> episodesBySeries;
    std::vector<MovieItem> ungroupedEpisodes;

    for (MovieItem& item : credits.items) {
        if (item.itemType == QStringLiteral("Movie")) {
            movies.push_back(std::move(item));
        } else if (item.itemType == QStringLiteral("Series")) {
            directShowCredits.insert(item.id);
            showsById.insert(item.id, std::move(item));
        } else if (item.itemType == QStringLiteral("Episode")) {
            if (item.seriesId.isEmpty())
                ungroupedEpisodes.push_back(std::move(item));
            else
                episodesBySeries[item.seriesId].push_back(std::move(item));
        }
    }

    struct SeasonGroup {
        QString seriesName;
        int seasonNumber = 0;
        std::vector<MovieItem> episodes;
    };
    std::vector<SeasonGroup> seasonGroups;

    for (auto seriesIt = episodesBySeries.begin(); seriesIt != episodesBySeries.end(); ++seriesIt) {
        const QString seriesId = seriesIt.key();
        std::vector<MovieItem>& episodes = seriesIt.value();
        const MovieItem related = relatedSeries.value(seriesId);
        const bool majorityCredit
            = related.recursiveItemCount > 0 && static_cast<int>(episodes.size()) * 2 > related.recursiveItemCount;
        if (directShowCredits.contains(seriesId) || majorityCredit) {
            if (!showsById.contains(seriesId)) {
                MovieItem show = related;
                if (show.id.isEmpty()) {
                    const MovieItem& sample = episodes.front();
                    show.id = seriesId;
                    show.title = sample.seriesName;
                    show.itemType = QStringLiteral("Series");
                    show.posterTag = sample.seriesPrimaryImageTag;
                }
                showsById.insert(seriesId, std::move(show));
            }
            continue;
        }

        QHash<int, std::vector<MovieItem>> seasons;
        for (MovieItem& episode : episodes)
            seasons[episode.seasonNumber].push_back(std::move(episode));
        for (auto seasonIt = seasons.begin(); seasonIt != seasons.end(); ++seasonIt) {
            std::vector<MovieItem>& seasonEpisodes = seasonIt.value();
            std::sort(seasonEpisodes.begin(), seasonEpisodes.end(),
                [](const MovieItem& left, const MovieItem& right) { return left.episodeNumber < right.episodeNumber; });
            const QString seriesName = !related.title.isEmpty() ? related.title : seasonEpisodes.front().seriesName;
            seasonGroups.push_back(SeasonGroup { seriesName, seasonIt.key(), std::move(seasonEpisodes) });
        }
    }

    auto byTitle = [](const MovieItem& left, const MovieItem& right) {
        return QString::localeAwareCompare(left.title, right.title) < 0;
    };
    std::sort(movies.begin(), movies.end(), byTitle);
    std::vector<MovieItem> shows;
    shows.reserve(static_cast<size_t>(showsById.size()));
    for (auto it = showsById.begin(); it != showsById.end(); ++it)
        shows.push_back(std::move(it.value()));
    std::sort(shows.begin(), shows.end(), byTitle);
    std::sort(seasonGroups.begin(), seasonGroups.end(), [](const SeasonGroup& left, const SeasonGroup& right) {
        const int nameOrder = QString::localeAwareCompare(left.seriesName, right.seriesName);
        return nameOrder != 0 ? nameOrder < 0 : left.seasonNumber < right.seasonNumber;
    });

    auto appendSection = [this](QString title, QString kind, std::vector<MovieItem> items) {
        if (items.empty())
            return;
        auto model = std::make_unique<MovieGridModel>();
        model->setMovies(std::move(items));
        m_prefetch->prefetchPosters(model->movies());
        m_personItemSections.push_back(PersonItemSection { std::move(title), std::move(kind), std::move(model) });
    };

    appendSection(QStringLiteral("Movies"), QStringLiteral("poster"), std::move(movies));
    appendSection(QStringLiteral("Shows"), QStringLiteral("poster"), std::move(shows));
    for (SeasonGroup& group : seasonGroups) {
        const QString seasonName
            = group.seasonNumber > 0 ? QStringLiteral("Season %1").arg(group.seasonNumber) : QStringLiteral("Specials");
        appendSection(QStringLiteral("%1 · %2").arg(group.seriesName, seasonName), QStringLiteral("landscape"),
            std::move(group.episodes));
    }
    appendSection(QStringLiteral("TV Episodes"), QStringLiteral("landscape"), std::move(ungroupedEpisodes));
}

void ContentModelController::clearPersonItems()
{
    m_personItemSections.clear();
}

void ContentModelController::prepareLinkedItem(const QString& itemId, const QString& title, const QString& itemType,
    const QString& seriesId, const QString& seriesName, const QString& seasonId)
{
    MovieItem item;
    item.id = itemId;
    item.title = title;
    item.itemType = itemType;
    item.seriesId = seriesId;
    item.seriesName = seriesName;
    item.seasonId = seasonId;
    m_linkedItems.setMovies({ std::move(item) });
}

void ContentModelController::updateResumeTicks(const QString& itemId, qint64 positionTicks)
{
    if (!itemId.isEmpty() && m_detailItem.id == itemId) {
        const qint64 ticks = normalizedResumeTicks(positionTicks, m_detailItem.runtimeTicks);
        if (m_detailItem.resumeTicks != ticks) {
            m_detailItem.resumeTicks = ticks;
            emit detailItemChanged();
        }
    }
    m_linkedItems.updateResumeTicks(itemId, positionTicks);
    m_detailSeasons.updateResumeTicks(itemId, positionTicks);
    m_detailSeasonOptions.updateResumeTicks(itemId, positionTicks);
    m_detailSimilarItems.updateResumeTicks(itemId, positionTicks);
    for (PersonItemSection& section : m_personItemSections)
        section.model->updateResumeTicks(itemId, positionTicks);
}

void ContentModelController::updateFavorite(const QString& itemId, bool favorite)
{
    if (!itemId.isEmpty() && m_detailItem.id == itemId && m_detailItem.favorite != favorite) {
        m_detailItem.favorite = favorite;
        emit detailItemChanged();
    }
    m_linkedItems.updateFavorite(itemId, favorite);
    m_detailSeasons.updateFavorite(itemId, favorite);
    m_detailSeasonOptions.updateFavorite(itemId, favorite);
    m_detailSimilarItems.updateFavorite(itemId, favorite);
    for (PersonItemSection& section : m_personItemSections)
        section.model->updateFavorite(itemId, favorite);
}

void ContentModelController::updatePlayed(const QString& itemId, bool played)
{
    if (!itemId.isEmpty() && m_detailItem.id == itemId
        && (m_detailItem.played != played || m_detailItem.resumeTicks != 0)) {
        m_detailItem.played = played;
        m_detailItem.resumeTicks = 0;
        emit detailItemChanged();
    }
    m_linkedItems.updatePlayed(itemId, played);
    m_detailSeasons.updatePlayed(itemId, played);
    m_detailSeasonOptions.updatePlayed(itemId, played);
    m_detailSimilarItems.updatePlayed(itemId, played);
    for (PersonItemSection& section : m_personItemSections)
        section.model->updatePlayed(itemId, played);
}

void ContentModelController::reset()
{
    m_detailRowsGeneration.invalidate();
    m_detailItemGeneration.invalidate();
    m_personItemsGeneration.invalidate();

    m_detailSeasons.clear();
    m_detailSeasonOptions.clear();
    m_detailSimilarItems.clear();
    clearPersonItems();
    m_linkedItems.clear();
    m_detailItem = {};
    m_detailRowsBusy = false;
    m_detailContextInitialIndex = 0;
    m_detailRowsPending = 0;
    m_personItemsBusy = false;

    emit detailRowsChanged();
    emit detailItemChanged();
    emit personItemsChanged();
}

void ContentModelController::finishDetailRowLoad(RequestGeneration::Token generation)
{
    if (!m_detailRowsGeneration.isCurrent(generation) || m_detailRowsPending <= 0) {
        return;
    }

    --m_detailRowsPending;
    if (m_detailRowsPending == 0) {
        m_detailRowsBusy = false;
        emit detailRowsChanged();
    }
}

} // namespace Spool
