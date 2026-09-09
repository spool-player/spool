#include "HomeModelController.h"

#include "../api/JellyfinApiFacade.h"
#include "../cache/DatabaseManager.h"
#include "../common/AsyncTask.h"
#include "../common/MetaJson.h"
#include "LibraryPrefetchController.h"
#include "LibraryQuery.h"

#include <QDebug>
#include <QHash>
#include <QJsonArray>
#include <QQmlEngine>
#include <QSet>
#include <QStringList>
#include <QVariantMap>

#include <algorithm>
#include <utility>

namespace JellyfinNative {

namespace {
    constexpr int kHomePayloadSchemaVersion = 11;

    // Cover art is square; cropping it to a poster or a thumbnail throws away
    // the edges of the artwork the way the album was meant to be seen.
    bool latestRowPrefersSquare(const LibraryItem& library, const std::vector<MovieItem>& items)
    {
        if (!items.empty()) {
            const QString& type = items.front().itemType;
            return type == QStringLiteral("MusicAlbum") || type == QStringLiteral("Audio")
                || type == QStringLiteral("MusicArtist");
        }
        return library.collectionType == QStringLiteral("music");
    }

    bool latestRowPrefersLandscape(const LibraryItem& library, const std::vector<MovieItem>& items)
    {
        if (!items.empty()) {
            const QString& type = items.front().itemType;
            if (type == QStringLiteral("Video"))
                return true;
            if (type == QStringLiteral("Movie") || type == QStringLiteral("Series") || type == QStringLiteral("Season")
                || type == QStringLiteral("Episode"))
                return false;
        }
        return library.collectionType != QStringLiteral("movies")
            && library.collectionType != QStringLiteral("tvshows");
    }

    QString latestRowKind(const LibraryItem& library, const std::vector<MovieItem>& items)
    {
        if (latestRowPrefersSquare(library, items))
            return QStringLiteral("square");
        return latestRowPrefersLandscape(library, items) ? QStringLiteral("landscape") : QStringLiteral("poster");
    }

    void removeItem(MovieGridModel& model, const QString& itemId)
    {
        std::vector<MovieItem> items = model.movies();
        const auto oldSize = items.size();
        items.erase(
            std::remove_if(items.begin(), items.end(), [&itemId](const MovieItem& item) { return item.id == itemId; }),
            items.end());
        if (items.size() == oldSize)
            return;
        model.setMovies(items);
    }

    std::vector<MovieItem> groupLatestItems(const LibraryItem& library, std::vector<MovieItem> items)
    {
        if (library.collectionType == QStringLiteral("music")) {
            QSet<QString> emittedAlbums;
            std::vector<MovieItem> grouped;
            grouped.reserve(items.size());
            for (MovieItem& item : items) {
                if (item.itemType != QStringLiteral("Audio") || item.albumId.isEmpty()) {
                    grouped.push_back(std::move(item));
                    continue;
                }
                if (emittedAlbums.contains(item.albumId))
                    continue;
                emittedAlbums.insert(item.albumId);
                MovieItem album;
                album.id = item.albumId;
                album.title = item.album.isEmpty() ? item.title : item.album;
                album.itemType = QStringLiteral("MusicAlbum");
                album.album = album.title;
                album.albumId = item.albumId;
                album.albumArtist = item.albumArtist;
                album.posterTag = item.albumPrimaryImageTag;
                grouped.push_back(std::move(album));
            }
            return grouped;
        }
        if (library.collectionType != QStringLiteral("tvshows"))
            return items;

        QHash<QString, QList<int>> episodeNumbers;
        for (const MovieItem& item : items) {
            if (item.itemType != QStringLiteral("Episode") || item.seriesId.isEmpty())
                continue;
            const QString seasonKey = !item.seasonId.isEmpty() ? item.seasonId : QString::number(item.seasonNumber);
            episodeNumbers[item.seriesId + QLatin1Char('/') + seasonKey].push_back(item.episodeNumber);
        }

        QSet<QString> emittedSeasons;
        std::vector<MovieItem> grouped;
        grouped.reserve(items.size());
        for (MovieItem& item : items) {
            if (item.itemType != QStringLiteral("Episode") || item.seriesId.isEmpty()) {
                grouped.push_back(std::move(item));
                continue;
            }

            const QString seasonKey = !item.seasonId.isEmpty() ? item.seasonId : QString::number(item.seasonNumber);
            const QString groupKey = item.seriesId + QLatin1Char('/') + seasonKey;
            if (emittedSeasons.contains(groupKey))
                continue;
            emittedSeasons.insert(groupKey);

            QList<int> numbers = episodeNumbers.value(groupKey);
            numbers.removeAll(0);
            std::sort(numbers.begin(), numbers.end());
            numbers.erase(std::unique(numbers.begin(), numbers.end()), numbers.end());
            MovieItem series;
            series.id = item.seriesId;
            series.title = item.seriesName.isEmpty() ? item.title : item.seriesName;
            series.posterTag = item.seriesPrimaryImageTag;
            series.itemType = QStringLiteral("Series");
            if (numbers.size() > 1 && numbers.back() - numbers.front() + 1 == numbers.size()) {
                series.episodeLabel = QStringLiteral("S%1 · E%2-E%3")
                                          .arg(item.seasonNumber, 2, 10, QLatin1Char('0'))
                                          .arg(numbers.front(), 2, 10, QLatin1Char('0'))
                                          .arg(numbers.back(), 2, 10, QLatin1Char('0'));
            }
            grouped.push_back(std::move(series));
        }
        return grouped;
    }

    QJsonArray movieArrayToJson(const std::vector<MovieItem>& items)
    {
        QJsonArray array;
        for (const MovieItem& item : items)
            array.push_back(metaToJson(item));
        return array;
    }

    std::vector<MovieItem> movieArrayFromJson(const QJsonArray& array)
    {
        std::vector<MovieItem> items;
        items.reserve(array.size());
        for (const QJsonValue& value : array) {
            MovieItem item = metaFromJson<MovieItem>(value.toObject());
            if (!item.id.isEmpty())
                items.push_back(std::move(item));
        }
        return items;
    }

} // namespace

HomeModelController::HomeModelController(
    DatabaseManager *database, JellyfinApiFacade *api, LibraryPrefetchController *prefetch, QObject *parent)
    : QObject(parent)
    , m_database(database)
    , m_api(api)
    , m_prefetch(prefetch)
{
}

QVariantList HomeModelController::latestLibraryRows() const
{
    QVariantList rows;
    rows.reserve(static_cast<qsizetype>(m_latestLibrarySections.size()));
    for (size_t row = 0; row < m_latestLibrarySections.size(); ++row) {
        const LatestLibrarySection& section = m_latestLibrarySections[row];
        if (!section.model || (section.model->rowCount() <= 0 && !m_refreshInFlight))
            continue;
        const std::vector<MovieItem>& items = section.model->movies();
        rows.push_back(QVariantMap {
            { QStringLiteral("rowIndex"), static_cast<int>(row) },
            { QStringLiteral("title"), QStringLiteral("Recently Added in %1").arg(section.library.name) },
            { QStringLiteral("libraryName"), section.library.name },
            { QStringLiteral("libraryId"), section.library.id },
            { QStringLiteral("collectionType"), section.library.collectionType },
            { QStringLiteral("kind"), latestRowKind(section.library, items) },
            { QStringLiteral("model"), QVariant::fromValue(static_cast<QObject *>(section.model.get())) },
        });
    }
    return rows;
}

bool HomeModelController::applyCachedPayload(const QJsonObject& payload)
{
    if (payload.isEmpty())
        return false;

    std::vector<PendingLatestLibrarySection> sections;
    const QJsonArray latestRows = payload.value(QStringLiteral("latestRows")).toArray();
    sections.reserve(latestRows.size());
    for (const QJsonValue& value : latestRows) {
        const QJsonObject row = value.toObject();
        PendingLatestLibrarySection section;
        section.order = row.value(QStringLiteral("order")).toInt();
        section.library = metaFromJson<LibraryItem>(row.value(QStringLiteral("library")).toObject());
        section.items = movieArrayFromJson(row.value(QStringLiteral("items")).toArray());
        if (!section.library.id.isEmpty() && !section.items.empty())
            sections.push_back(std::move(section));
    }

    m_resumeItems.setMovies(movieArrayFromJson(payload.value(QStringLiteral("resumeItems")).toArray()));
    m_nextUpItems.setMovies(movieArrayFromJson(payload.value(QStringLiteral("nextUpItems")).toArray()));
    if (updateLatestLibraryRows(std::move(sections)))
        emit latestLibraryRowsChanged();
    return m_resumeItems.rowCount() > 0 || m_nextUpItems.rowCount() > 0 || !m_latestLibrarySections.empty();
}

void HomeModelController::loadCachedPayload()
{
    Async::runScoped(
        this, loadCachedPayloadAsync(), []() {},
        [](const std::exception_ptr& error) {
            qWarning() << "home: warm payload cache failed" << exceptionMessage(error);
        },
        "home cache");
}

QCoro::Task<void> HomeModelController::loadCachedPayloadAsync()
{
    if (!m_database)
        co_return;
    const QString key = payloadCacheKey();
    if (key.isEmpty())
        co_return;
    const QJsonObject payload = co_await m_database->loadHomePayloadAsync(key, kHomePayloadSchemaVersion);
    if (applyCachedPayload(payload))
        qInfo() << "home: warm payload cache applied" << key;
}

QString HomeModelController::payloadCacheKey() const
{
    if (!m_api)
        return {};
    const AuthSession session = m_api->session();
    const QString userKey = session.userId.isEmpty() ? session.userName : session.userId;
    const QString serverKey = session.serverId.isEmpty() ? m_api->serverUrl() : session.serverId;
    return userKey.isEmpty() || serverKey.isEmpty() ? QString() : QStringLiteral("%1/%2").arg(serverKey, userKey);
}

void HomeModelController::saveCachedPayload(const QJsonObject& payload)
{
    const QString key = payloadCacheKey();
    if (m_database && !payload.isEmpty() && !key.isEmpty())
        m_database->saveHomePayload(key, kHomePayloadSchemaVersion, payload);
}

void HomeModelController::refresh(const std::vector<LibraryItem>& libraries)
{
    if (!m_api || m_api->session().accessToken.isEmpty())
        return;
    if (libraries.empty())
        return;
    if (m_loaded || m_refreshInFlight)
        return;

    const RequestGeneration::Token generation = m_generation.next();
    m_refreshInFlight = true;
    if (m_latestLibrarySections.empty()) {
        std::vector<PendingLatestLibrarySection> sections;
        for (const LibraryItem& library : libraries) {
            if (supportsLatestLibraryRow(library))
                sections.push_back({ static_cast<int>(sections.size()), library, {} });
        }
        updateLatestLibraryRows(std::move(sections));
        emit latestLibraryRowsChanged();
    }
    emit loadingChanged();
    m_prefetch->stop();
    Async::runScoped(
        this, refreshAsync(libraries, generation), []() {},
        [this, generation](const std::exception_ptr& error) {
            if (!m_generation.isCurrent(generation))
                return;
            m_refreshInFlight = false;
            emit loadingChanged();
            emit latestLibraryRowsChanged();
            qWarning() << "home: refresh failed" << exceptionMessage(error);
        },
        "home refresh");
}

QCoro::Task<void> HomeModelController::refreshAsync(
    std::vector<LibraryItem> libraries, RequestGeneration::Token generation)
{
    std::vector<LibraryItem> latestLibraries;
    latestLibraries.reserve(libraries.size());
    for (const LibraryItem& library : libraries) {
        if (supportsLatestLibraryRow(library))
            latestLibraries.push_back(library);
    }

    auto resumeTask = m_api->fetchResumeItems();
    auto nextUpTask = m_api->fetchNextUpEpisodes();
    std::vector<QCoro::Task<std::vector<MovieItem>>> latestTasks;
    latestTasks.reserve(latestLibraries.size());
    for (const LibraryItem& library : latestLibraries)
        latestTasks.push_back(fetchLatestLibraryItems(library));

    std::vector<MovieItem> resumeItems;
    try {
        resumeItems = co_await resumeTask;
        qInfo() << "home: resume items" << resumeItems.size();
    } catch (const std::exception&) {
        qWarning() << "home: resume fetch failed";
    }
    if (!m_generation.isCurrent(generation))
        co_return;

    std::vector<MovieItem> nextUpItems;
    try {
        nextUpItems = co_await nextUpTask;
        qInfo() << "home: next-up items" << nextUpItems.size();
    } catch (const std::exception&) {
        qWarning() << "home: next-up fetch failed";
    }
    if (!m_generation.isCurrent(generation))
        co_return;

    m_resumeItems.setMovies(resumeItems);
    m_nextUpItems.setMovies(nextUpItems);

    std::vector<PendingLatestLibrarySection> latestSections;
    latestSections.reserve(latestLibraries.size());
    for (int order = 0; order < static_cast<int>(latestLibraries.size()); ++order) {
        const LibraryItem& library = latestLibraries[static_cast<size_t>(order)];
        try {
            std::vector<MovieItem> items = co_await latestTasks[static_cast<size_t>(order)];
            qInfo() << "home: latest items" << items.size();
            if (!items.empty())
                latestSections.push_back({ order, library, std::move(items) });
        } catch (const std::exception&) {
            qWarning() << "home: latest fetch failed";
        }
        if (!m_generation.isCurrent(generation))
            co_return;
    }

    m_refreshInFlight = false;
    m_loaded = true;
    saveCachedPayload(payloadFromSections(resumeItems, nextUpItems, latestSections));
    const bool latestRowsChanged = updateLatestLibraryRows(std::move(latestSections));
    emit loadingChanged();

    m_prefetch->prefetchPosters(resumeItems, 0, 12, LibraryPrefetchController::ImageKind::Landscape);
    m_prefetch->prefetchPosters(nextUpItems, 0, 12, LibraryPrefetchController::ImageKind::Landscape);
    for (const LatestLibrarySection& section : m_latestLibrarySections) {
        if (!section.model)
            continue;
        m_prefetch->prefetchPosters(section.model->movies(), 0, 12,
            latestRowPrefersLandscape(section.library, section.model->movies())
                ? LibraryPrefetchController::ImageKind::Landscape
                : LibraryPrefetchController::ImageKind::Poster);
    }
    m_prefetch->schedule(libraries, m_recentLibraryIds);
    if (latestRowsChanged || !m_latestLibrarySections.empty())
        emit latestLibraryRowsChanged();
}

QCoro::Task<std::vector<MovieItem>> HomeModelController::fetchLatestLibraryItems(LibraryItem library)
{
    constexpr int kTargetItems = 20;
    constexpr int kMaximumRawItems = 200;
    constexpr int kInitialTvRawItems = 120;

    int limit = library.collectionType == QStringLiteral("tvshows") ? kInitialTvRawItems : kTargetItems;
    while (limit <= kMaximumRawItems) {
        std::vector<MovieItem> rawItems = co_await m_api->fetchLatestItems(library.id, limit);
        const int rawCount = static_cast<int>(rawItems.size());
        std::vector<MovieItem> groupedItems = groupLatestItems(library, std::move(rawItems));
        qInfo() << "home: latest fill raw=" << rawCount << "grouped=" << groupedItems.size() << "limit=" << limit;
        if (groupedItems.size() >= kTargetItems) {
            groupedItems.resize(kTargetItems);
            co_return groupedItems;
        }
        if (rawCount < limit || limit == kMaximumRawItems)
            co_return groupedItems;
        limit = kMaximumRawItems;
    }
    co_return std::vector<MovieItem> {};
}

void HomeModelController::recordLibraryUse(const LibraryItem& library)
{
    if (library.id.isEmpty())
        return;

    m_recentLibraryIds.removeAll(library.id);
    m_recentLibraryIds.prepend(library.id);
    while (m_recentLibraryIds.size() > 12)
        m_recentLibraryIds.removeLast();
}

void HomeModelController::upsertResumeItem(MovieItem item, qint64 positionTicks)
{
    if (item.id.isEmpty())
        return;

    item.resumeTicks = normalizedResumeTicks(positionTicks, item.runtimeTicks);
    item.played = false;
    if (!isMeaningfulResumePosition(item.resumeTicks, item.runtimeTicks)) {
        updateResumeTicks(item.id, item.resumeTicks);
        return;
    }

    const auto current = m_resumeItems.movies();
    if (!current.empty() && current.front().id == item.id) {
        m_resumeItems.updateResumeTicks(item.id, item.resumeTicks);
        return;
    }

    std::vector<MovieItem> items = current;
    items.erase(std::remove_if(items.begin(), items.end(),
                    [&item](const MovieItem& candidate) { return candidate.id == item.id; }),
        items.end());
    items.insert(items.begin(), item);
    if (items.size() > 24)
        items.resize(24);

    m_resumeItems.setMovies(items);
    m_prefetch->prefetchPosters(
        std::vector<MovieItem> { item }, 0, 12, LibraryPrefetchController::ImageKind::Landscape);
}

void HomeModelController::updateResumeTicks(const QString& itemId, qint64 positionTicks)
{
    m_resumeItems.updateResumeTicks(itemId, positionTicks);
    m_resumeItems.removeUnresumable();
    m_nextUpItems.updateResumeTicks(itemId, positionTicks);
    for (LatestLibrarySection& section : m_latestLibrarySections) {
        if (section.model)
            section.model->updateResumeTicks(itemId, positionTicks);
    }
}

void HomeModelController::updateFavorite(const QString& itemId, bool favorite)
{
    m_resumeItems.updateFavorite(itemId, favorite);
    m_nextUpItems.updateFavorite(itemId, favorite);
    for (LatestLibrarySection& section : m_latestLibrarySections) {
        if (section.model)
            section.model->updateFavorite(itemId, favorite);
    }
}

void HomeModelController::updatePlayed(const QString& itemId, bool played)
{
    m_resumeItems.updatePlayed(itemId, played);
    m_resumeItems.removeUnresumable();
    if (played)
        removeItem(m_nextUpItems, itemId);
    else
        m_nextUpItems.updatePlayed(itemId, played);
    for (LatestLibrarySection& section : m_latestLibrarySections) {
        if (section.model)
            section.model->updatePlayed(itemId, played);
    }
}

void HomeModelController::reset()
{
    m_generation.invalidate();
    m_refreshInFlight = false;
    m_loaded = false;
    m_prefetch->stop();
    m_resumeItems.clear();
    m_nextUpItems.clear();
    m_recentLibraryIds.clear();
    m_latestLibrarySections.clear();
    emit latestLibraryRowsChanged();
    emit loadingChanged();
}

bool HomeModelController::updateLatestLibraryRows(std::vector<PendingLatestLibrarySection> sections)
{
    std::sort(sections.begin(), sections.end(),
        [](const PendingLatestLibrarySection& left, const PendingLatestLibrarySection& right) {
            return left.order < right.order;
        });

    const bool sameRows = sections.size() == m_latestLibrarySections.size()
        && std::equal(sections.cbegin(), sections.cend(), m_latestLibrarySections.cbegin(),
            [](const PendingLatestLibrarySection& next, const LatestLibrarySection& current) {
                return current.model && next.library.id == current.library.id;
            });
    if (sameRows) {
        bool metadataChanged = false;
        for (size_t index = 0; index < sections.size(); ++index) {
            PendingLatestLibrarySection& next = sections[index];
            LatestLibrarySection& current = m_latestLibrarySections[index];
            metadataChanged = metadataChanged || current.library.name != next.library.name
                || current.library.collectionType != next.library.collectionType
                || latestRowKind(current.library, current.model->movies()) != latestRowKind(next.library, next.items);
            current.order = next.order;
            current.library = std::move(next.library);
            current.model->setMovies(std::move(next.items));
        }
        return metadataChanged;
    }

    m_latestLibrarySections.clear();
    m_latestLibrarySections.reserve(sections.size());
    for (PendingLatestLibrarySection& pending : sections) {
        auto model = std::make_unique<MovieGridModel>();
        QQmlEngine::setObjectOwnership(model.get(), QQmlEngine::CppOwnership);
        model->setMovies(std::move(pending.items));
        m_latestLibrarySections.push_back({ pending.order, std::move(pending.library), std::move(model) });
    }
    return true;
}

QJsonObject HomeModelController::payloadFromSections(const std::vector<MovieItem>& resumeItems,
    const std::vector<MovieItem>& nextUpItems, const std::vector<PendingLatestLibrarySection>& sections) const
{
    QJsonArray latestRows;
    for (const PendingLatestLibrarySection& section : sections) {
        if (section.library.id.isEmpty() || section.items.empty())
            continue;
        latestRows.push_back(QJsonObject {
            { QStringLiteral("order"), section.order },
            { QStringLiteral("library"), metaToJson(section.library) },
            { QStringLiteral("items"), movieArrayToJson(section.items) },
        });
    }
    return {
        { QStringLiteral("schemaVersion"), kHomePayloadSchemaVersion },
        { QStringLiteral("resumeItems"), movieArrayToJson(resumeItems) },
        { QStringLiteral("nextUpItems"), movieArrayToJson(nextUpItems) },
        { QStringLiteral("latestRows"), latestRows },
    };
}

} // namespace JellyfinNative
