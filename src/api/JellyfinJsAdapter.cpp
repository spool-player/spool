#include "JellyfinJsAdapter.h"

#include "../common/MetaJson.h"
#include "../provider/ProviderMediaPage.h"
#include "../provider/ProviderRegistry.h"
#include "PlaybackNegotiation.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>

#include <algorithm>

namespace JellyfinNative {

class JellyfinJsAdapter::Playback final : public PlaybackSource {
    Q_OBJECT

public:
    explicit Playback(JellyfinJsAdapter *adapter, QObject *parent = nullptr)
        : PlaybackSource(parent)
        , m_adapter(adapter)
    {
    }

    QByteArray mediaRequestHeaders() const override
    {
        return m_adapter->mediaRequestHeaders();
    }

    QUrl mediaOrigin() const override
    {
        return m_adapter->mediaOrigin();
    }

    int playbackParallelRequests() const override
    {
        return 2;
    }

    bool signedIn() const override
    {
        return m_adapter->signedIn();
    }

    QString trickplayTileUrl(const QString& itemId, int width, int tileIndex) const override
    {
        return m_adapter->trickplayTileUrl(itemId, width, tileIndex);
    }

    QCoro::Task<PlaybackSession> resolvePlayback(MovieItem item, bool forceTranscode) override
    {
        return m_adapter->resolvePlayback(std::move(item), forceTranscode);
    }

    QCoro::Task<std::vector<MediaSegment>> fetchMediaSegments(QString itemId) override
    {
        return m_adapter->fetchMediaSegments(std::move(itemId));
    }

    QCoro::Task<std::vector<MovieItem>> fetchSeriesEpisodes(QString seriesId) override
    {
        return m_adapter->fetchEpisodes(std::move(seriesId));
    }

    QCoro::Task<void> reportPlaybackStart(PlaybackSession session, double playbackRate, int volume, bool muted) override
    {
        return m_adapter->reportPlaybackStart(std::move(session), playbackRate, volume, muted);
    }

    QCoro::Task<void> reportPlaybackProgress(PlaybackSession session, qint64 positionTicks, bool paused,
        double playbackRate, int volume, bool muted) override
    {
        return m_adapter->reportPlaybackProgress(
            std::move(session), positionTicks, paused, playbackRate, volume, muted);
    }

    QCoro::Task<void> reportPlaybackStopped(
        PlaybackSession session, qint64 positionTicks, bool failed, double playbackRate) override
    {
        return m_adapter->reportPlaybackStopped(std::move(session), positionTicks, failed, playbackRate);
    }

    void emitCredentialsChanged()
    {
        emit credentialsChanged();
    }

private:
    JellyfinJsAdapter *m_adapter;
};

JellyfinJsAdapter::JellyfinJsAdapter(ProviderRegistry *registry, QObject *parent)
    : QObject(parent)
    , m_registry(registry)
    , m_playback(new Playback(this, this))
{
}

JellyfinJsAdapter::~JellyfinJsAdapter() = default;

void JellyfinJsAdapter::configure(const QString& sourceId, const QString& serverUrl, const QString& sessionToken)
{
    const bool changed = m_sourceId != sourceId || m_sessionToken != sessionToken;
    m_sourceId = sourceId;
    m_serverUrl = serverUrl;
    m_sessionToken = sessionToken;
    if (changed && m_playback)
        m_playback->emitCredentialsChanged();
}

void JellyfinJsAdapter::clear()
{
    m_sourceId.clear();
    m_serverUrl.clear();
    m_sessionToken.clear();
    m_bitrateOverride = 0;
    m_heightOverride = 0;
    if (m_playback)
        m_playback->emitCredentialsChanged();
}

void JellyfinJsAdapter::setDeviceId(const QString& deviceId)
{
    m_deviceId = deviceId;
}

void JellyfinJsAdapter::setVideoCodecCapabilities(QStringList videoCodecs, bool restrictVideoCodecs)
{
    m_videoCodecs = std::move(videoCodecs);
    m_restrictVideoCodecs = restrictVideoCodecs;
}

PlaybackSource *JellyfinJsAdapter::playback()
{
    return m_playback;
}

bool JellyfinJsAdapter::signedIn() const
{
    return !m_sourceId.isEmpty();
}

QString JellyfinJsAdapter::libraryScopeKey() const
{
    return m_sourceId;
}

QCoro::Task<PagedMovieItems> JellyfinJsAdapter::fetchBrowsePage(
    BrowseDescriptor descriptor, int startIndex, int limit, QVariantMap queryOptions)
{
    if (!descriptor.isValid() || m_sourceId.isEmpty())
        co_return PagedMovieItems { {}, 0, std::max(0, startIndex), std::clamp(limit, 1, 200) };

    const int effectiveStart = std::max(0, startIndex);
    const int effectiveLimit = std::clamp(limit, 1, 100);

    QString operation = QStringLiteral("browse");
    QVariantMap args;
    args.insert(QStringLiteral("cursor"), QString::number(effectiveStart));
    args.insert(QStringLiteral("limit"), effectiveLimit);

    switch (descriptor.kind) {
    case BrowseKind::Library:
        if (!descriptor.id.isEmpty())
            args.insert(QStringLiteral("parentId"), descriptor.id);
        if (!descriptor.collectionType.isEmpty())
            args.insert(QStringLiteral("collectionType"), descriptor.collectionType);
        break;
    case BrowseKind::FolderChildren:
        args.insert(QStringLiteral("parentId"), descriptor.id);
        args.insert(QStringLiteral("recursive"), false);
        break;
    case BrowseKind::BoxSet:
        args.insert(QStringLiteral("parentId"), descriptor.id);
        args.insert(QStringLiteral("recursive"), false);
        break;
    case BrowseKind::Genre:
        if (!descriptor.id.isEmpty())
            args.insert(QStringLiteral("parentId"), descriptor.id);
        if (!descriptor.collectionType.isEmpty())
            args.insert(QStringLiteral("collectionType"), descriptor.collectionType);
        args.insert(
            QStringLiteral("filters"), QVariantMap { { QStringLiteral("Genres"), QVariantList { descriptor.name } } });
        break;
    case BrowseKind::Studio:
        if (!descriptor.id.isEmpty())
            args.insert(QStringLiteral("parentId"), descriptor.id);
        if (!descriptor.collectionType.isEmpty())
            args.insert(QStringLiteral("collectionType"), descriptor.collectionType);
        args.insert(
            QStringLiteral("filters"), QVariantMap { { QStringLiteral("StudioIds"), QVariantList { descriptor.id } } });
        break;
    case BrowseKind::SeriesSeasons:
        operation = QStringLiteral("seasons");
        args.insert(QStringLiteral("seriesId"), descriptor.seriesId.isEmpty() ? descriptor.id : descriptor.seriesId);
        break;
    case BrowseKind::SeasonEpisodes:
        operation = QStringLiteral("episodes");
        args.insert(QStringLiteral("seriesId"), descriptor.seriesId);
        if (!descriptor.seasonId.isEmpty())
            args.insert(QStringLiteral("seasonId"), descriptor.seasonId);
        break;
    case BrowseKind::Person:
        operation = QStringLiteral("personItems");
        args.insert(QStringLiteral("personId"), descriptor.id);
        break;
    case BrowseKind::Playlist:
    case BrowseKind::ArtistAlbums:
    case BrowseKind::None:
        break;
    }

    if (descriptor.kind == BrowseKind::Library) {
        if (queryOptions.contains(QStringLiteral("sortBy")))
            args.insert(QStringLiteral("sortBy"), queryOptions.value(QStringLiteral("sortBy")));
        if (queryOptions.contains(QStringLiteral("sortOrder")))
            args.insert(QStringLiteral("sortOrder"), queryOptions.value(QStringLiteral("sortOrder")));
        if (queryOptions.contains(QStringLiteral("filters"))) {
            const QVariantMap rawFilters = queryOptions.value(QStringLiteral("filters")).toMap();
            QVariantMap filtered;
            static const QStringList allowed = { QStringLiteral("Filters"), QStringLiteral("Genres"),
                QStringLiteral("OfficialRatings"), QStringLiteral("Tags"), QStringLiteral("Years"),
                QStringLiteral("StudioIds"), QStringLiteral("SeriesStatus"), QStringLiteral("VideoTypes"),
                QStringLiteral("IsHd"), QStringLiteral("Is4K"), QStringLiteral("Is3D"), QStringLiteral("HasSubtitles"),
                QStringLiteral("HasTrailer"), QStringLiteral("IsMissing"), QStringLiteral("IsUnaired"),
                QStringLiteral("NameStartsWith"), QStringLiteral("NameLessThan") };
            for (auto it = rawFilters.begin(); it != rawFilters.end(); ++it) {
                if (allowed.contains(it.key()))
                    filtered.insert(it.key(), it.value());
            }
            if (!filtered.isEmpty())
                args.insert(QStringLiteral("filters"), filtered);
        }
    }

    ProviderMediaPage page
        = co_await m_registry->callSourceMediaPage(m_sourceId, operation, args, QString(), effectiveLimit);

    std::vector<MovieItem> items;
    items.reserve(page.items.size());
    for (auto& row : page.items)
        items.push_back(std::move(row.media));

    if (descriptor.kind == BrowseKind::SeriesSeasons) {
        const QString seriesId = descriptor.seriesId.isEmpty() ? descriptor.id : descriptor.seriesId;
        for (MovieItem& item : items) {
            if (item.seriesId.isEmpty())
                item.seriesId = seriesId;
        }
    }

    const int totalCount = page.total.has_value()
        ? static_cast<int>(page.total.value())
        : (page.exhausted ? effectiveStart + static_cast<int>(items.size())
                          : effectiveStart + static_cast<int>(items.size()) + 1);

    co_return PagedMovieItems { std::move(items), totalCount, effectiveStart, effectiveLimit };
}

QCoro::Task<MovieItem> JellyfinJsAdapter::fetchItemDetails(QString itemId)
{
    if (itemId.isEmpty() || m_sourceId.isEmpty())
        co_return MovieItem {};

    QVariantMap res = co_await m_registry->callSource(
        m_sourceId, QStringLiteral("details"), { { QStringLiteral("itemId"), itemId } });
    const QVariantMap row = res.value(QStringLiteral("item")).toMap();
    MovieItem media;
    media.id = row.value(QStringLiteral("id")).toString();
    media.title = row.value(QStringLiteral("title")).toString();
    media.sortName = row.value(QStringLiteral("sortName")).toString();
    if (media.sortName.isEmpty())
        media.sortName = media.title;
    media.itemType = row.value(QStringLiteral("type")).toString();
    media.overview = row.value(QStringLiteral("overview")).toString();
    media.posterTag = row.value(QStringLiteral("posterTag")).toString();
    media.backdropTag = row.value(QStringLiteral("backdropTag")).toString();
    media.logoTag = row.value(QStringLiteral("logoTag")).toString();
    media.seriesId = row.value(QStringLiteral("seriesId")).toString();
    media.seasonId = row.value(QStringLiteral("seasonId")).toString();
    media.seriesName = row.value(QStringLiteral("seriesName")).toString();
    media.year = row.value(QStringLiteral("year")).toInt();
    media.seasonNumber = row.value(QStringLiteral("season")).toInt();
    media.episodeNumber = row.value(QStringLiteral("episode")).toInt();
    media.runtimeTicks = row.value(QStringLiteral("runtimeTicks")).toLongLong();
    media.resumeTicks = row.value(QStringLiteral("resumeTicks")).toLongLong();
    media.favorite = row.value(QStringLiteral("favorite")).toBool();
    media.played = row.value(QStringLiteral("played")).toBool();
    media.genres = row.value(QStringLiteral("genres")).toStringList();
    media.tags = row.value(QStringLiteral("tags")).toStringList();
    media.studios = row.value(QStringLiteral("studios")).toStringList();
    media.officialRating = row.value(QStringLiteral("officialRating")).toString();
    media.communityRating = row.value(QStringLiteral("communityRating")).toDouble();

    const QVariantList people = row.value(QStringLiteral("people")).toList();
    for (const auto& pVal : people) {
        const QVariantMap p = pVal.toMap();
        PersonItem person;
        person.id = p.value(QStringLiteral("id")).toString();
        person.name = p.value(QStringLiteral("name")).toString();
        person.type = p.value(QStringLiteral("type")).toString();
        person.role = p.value(QStringLiteral("role")).toString();
        person.imageTag = p.value(QStringLiteral("imageTag")).toString();
        media.people.append(std::move(person));
    }
    co_return media;
}

QCoro::Task<std::vector<MovieItem>> JellyfinJsAdapter::fetchSeasons(QString seriesId)
{
    const BrowseDescriptor desc = BrowseDescriptor::seriesSeasons(seriesId);
    PagedMovieItems page = co_await fetchBrowsePage(desc, 0, 100);
    for (auto& item : page.items) {
        if (item.seriesId.isEmpty())
            item.seriesId = seriesId;
    }
    co_return page.items;
}

QCoro::Task<std::vector<MovieItem>> JellyfinJsAdapter::fetchEpisodes(QString seriesId, QString seasonId)
{
    const BrowseDescriptor desc = BrowseDescriptor::seasonEpisodes(seriesId, seasonId);
    PagedMovieItems page = co_await fetchBrowsePage(desc, 0, 100);
    co_return page.items;
}

QCoro::Task<std::vector<MovieItem>> JellyfinJsAdapter::fetchResumeItems(int limit)
{
    if (m_sourceId.isEmpty())
        co_return {};
    const int effectiveLimit = std::clamp(limit, 1, 60);
    ProviderMediaPage page = co_await m_registry->callSourceMediaPage(m_sourceId, QStringLiteral("resume"),
        { { QStringLiteral("limit"), effectiveLimit } }, QString(), effectiveLimit);
    std::vector<MovieItem> items;
    items.reserve(page.items.size());
    for (auto& row : page.items)
        items.push_back(std::move(row.media));
    co_return items;
}

QCoro::Task<std::vector<MovieItem>> JellyfinJsAdapter::fetchNextUpEpisodes(int limit)
{
    if (m_sourceId.isEmpty())
        co_return {};
    const int effectiveLimit = std::clamp(limit, 1, 60);
    ProviderMediaPage page = co_await m_registry->callSourceMediaPage(m_sourceId, QStringLiteral("nextUp"),
        { { QStringLiteral("limit"), effectiveLimit } }, QString(), effectiveLimit);
    std::vector<MovieItem> items;
    items.reserve(page.items.size());
    for (auto& row : page.items)
        items.push_back(std::move(row.media));
    co_return items;
}

QCoro::Task<std::vector<MovieItem>> JellyfinJsAdapter::fetchLatestItems(QString parentId, int limit)
{
    if (m_sourceId.isEmpty())
        co_return {};
    const int effectiveLimit = std::clamp(limit, 1, 60);
    QVariantMap args { { QStringLiteral("limit"), effectiveLimit } };
    if (!parentId.isEmpty())
        args.insert(QStringLiteral("parentId"), parentId);
    ProviderMediaPage page = co_await m_registry->callSourceMediaPage(
        m_sourceId, QStringLiteral("latest"), args, QString(), effectiveLimit);
    std::vector<MovieItem> items;
    items.reserve(page.items.size());
    for (auto& row : page.items)
        items.push_back(std::move(row.media));
    co_return items;
}

QCoro::Task<std::vector<MovieItem>> JellyfinJsAdapter::fetchSimilarItems(QString itemId, int limit)
{
    if (itemId.isEmpty() || m_sourceId.isEmpty())
        co_return {};
    const int effectiveLimit = std::clamp(limit, 1, 60);
    ProviderMediaPage page = co_await m_registry->callSourceMediaPage(m_sourceId, QStringLiteral("similar"),
        { { QStringLiteral("itemId"), itemId }, { QStringLiteral("limit"), effectiveLimit } }, QString(),
        effectiveLimit);
    std::vector<MovieItem> items;
    items.reserve(page.items.size());
    for (auto& row : page.items)
        items.push_back(std::move(row.media));
    co_return items;
}

QCoro::Task<PersonCredits> JellyfinJsAdapter::fetchItemsByPerson(QString personId, int maximumItems)
{
    PersonCredits credits;
    if (personId.isEmpty() || m_sourceId.isEmpty())
        co_return credits;
    const int effectiveLimit = std::clamp(maximumItems, 1, 100);
    ProviderMediaPage page = co_await m_registry->callSourceMediaPage(m_sourceId, QStringLiteral("personItems"),
        { { QStringLiteral("personId"), personId }, { QStringLiteral("limit"), effectiveLimit } }, QString(),
        effectiveLimit);
    credits.items.reserve(page.items.size());
    for (auto& row : page.items)
        credits.items.push_back(std::move(row.media));
    co_return credits;
}

QCoro::Task<std::vector<LibraryItem>> JellyfinJsAdapter::fetchLibraries()
{
    if (m_sourceId.isEmpty())
        co_return {};
    QVariantMap res = co_await m_registry->callSource(m_sourceId, QStringLiteral("libraries"));
    std::vector<LibraryItem> libraries;
    const QVariantList items = res.value(QStringLiteral("items")).toList();
    libraries.reserve(items.size());
    for (const auto& itemVal : items) {
        const QVariantMap row = itemVal.toMap();
        LibraryItem item;
        item.id = row.value(QStringLiteral("id")).toString();
        item.name = row.value(QStringLiteral("title")).toString();
        item.collectionType = row.value(QStringLiteral("collectionType")).toString();
        item.imageTag = row.value(QStringLiteral("posterTag")).toString();
        libraries.push_back(std::move(item));
    }
    co_return libraries;
}

QCoro::Task<QVariantMap> JellyfinJsAdapter::fetchLibraryFilterOptions(QString libraryId, QString collectionType)
{
    if (libraryId.isEmpty() || m_sourceId.isEmpty())
        co_return QVariantMap {};
    QVariantMap args { { QStringLiteral("parentId"), libraryId } };
    if (!collectionType.isEmpty())
        args.insert(QStringLiteral("collectionType"), collectionType);
    QVariantMap filters = co_await m_registry->callSource(m_sourceId, QStringLiteral("filterOptions"), args);
    QVariantMap options;
    if (filters.contains(QStringLiteral("Genres")))
        options.insert(QStringLiteral("genres"), filters.value(QStringLiteral("Genres")));
    if (filters.contains(QStringLiteral("OfficialRatings")))
        options.insert(QStringLiteral("officialRatings"), filters.value(QStringLiteral("OfficialRatings")));
    if (filters.contains(QStringLiteral("Tags")))
        options.insert(QStringLiteral("tags"), filters.value(QStringLiteral("Tags")));
    if (filters.contains(QStringLiteral("Years")))
        options.insert(QStringLiteral("years"), filters.value(QStringLiteral("Years")));
    co_return options;
}

QCoro::Task<std::vector<MovieItem>> JellyfinJsAdapter::fetchItemsByIds(QStringList itemIds)
{
    if (itemIds.isEmpty() || m_sourceId.isEmpty())
        co_return {};
    QVariantMap filters { { QStringLiteral("Ids"), itemIds.join(QLatin1Char(',')) } };
    const int effectiveLimit = std::clamp(static_cast<int>(itemIds.size()), 1, 100);
    QVariantMap args { { QStringLiteral("filters"), filters }, { QStringLiteral("limit"), effectiveLimit } };
    ProviderMediaPage page = co_await m_registry->callSourceMediaPage(
        m_sourceId, QStringLiteral("browse"), args, QString(), effectiveLimit);
    std::vector<MovieItem> items;
    items.reserve(page.items.size());
    for (auto& row : page.items)
        items.push_back(std::move(row.media));
    co_return items;
}

QCoro::Task<std::vector<MovieItem>> JellyfinJsAdapter::searchItems(QString searchTerm, int limit)
{
    if (searchTerm.isEmpty() || m_sourceId.isEmpty())
        co_return {};
    const int effectiveLimit = std::clamp(limit, 1, 100);
    ProviderMediaPage page = co_await m_registry->callSourceMediaPage(m_sourceId, QStringLiteral("search"),
        { { QStringLiteral("query"), searchTerm }, { QStringLiteral("limit"), effectiveLimit } }, QString(),
        effectiveLimit);
    std::vector<MovieItem> items;
    items.reserve(page.items.size());
    for (auto& row : page.items)
        items.push_back(std::move(row.media));
    co_return items;
}

QCoro::Task<std::vector<MovieItem>> JellyfinJsAdapter::fetchSearchSuggestions(int limit)
{
    return fetchResumeItems(limit);
}

QCoro::Task<void> JellyfinJsAdapter::setItemFavorite(QString itemId, bool favorite)
{
    if (itemId.isEmpty() || m_sourceId.isEmpty())
        co_return;
    co_await m_registry->callSource(m_sourceId, QStringLiteral("favorite"),
        { { QStringLiteral("itemId"), itemId }, { QStringLiteral("value"), favorite } });
}

QCoro::Task<void> JellyfinJsAdapter::setItemPlayed(QString itemId, bool played)
{
    if (itemId.isEmpty() || m_sourceId.isEmpty())
        co_return;
    co_await m_registry->callSource(m_sourceId, QStringLiteral("played"),
        { { QStringLiteral("itemId"), itemId }, { QStringLiteral("value"), played } });
}

QCoro::Task<void> JellyfinJsAdapter::setItemPlaybackPosition(QString itemId, qint64 positionTicks)
{
    if (itemId.isEmpty() || m_sourceId.isEmpty())
        co_return;
    co_await m_registry->callSource(m_sourceId, QStringLiteral("progress"),
        { { QStringLiteral("itemId"), itemId }, { QStringLiteral("positionTicks"), QString::number(positionTicks) } });
}

QString JellyfinJsAdapter::imageUrl(const ImageRequest& request) const
{
    const QString origin = m_serverUrl;
    if (origin.isEmpty() || request.itemId.isEmpty() || request.tag.isEmpty() || request.imageType.isEmpty())
        return {};
    QUrl url = serverUrlWithPath(
        origin, { QStringLiteral("Items"), request.itemId, QStringLiteral("Images"), request.imageType });
    QUrlQuery query;
    if (request.fillWidth > 0 && request.fillHeight > 0) {
        query.addQueryItem(QStringLiteral("fillWidth"), QString::number(request.fillWidth));
        query.addQueryItem(QStringLiteral("fillHeight"), QString::number(request.fillHeight));
    } else {
        query.addQueryItem(QStringLiteral("maxWidth"), QString::number(request.maxWidth));
    }
    query.addQueryItem(QStringLiteral("quality"), QString::number(request.quality));
    query.addQueryItem(QStringLiteral("format"), request.format);
    query.addQueryItem(QStringLiteral("tag"), request.tag);
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

QCoro::Task<PlaybackSession> JellyfinJsAdapter::resolvePlayback(MovieItem movie, bool forceTranscode)
{
    if (m_sourceId.isEmpty())
        throw std::runtime_error("playback_not_ready");

    QString variantId;
    if (!movie.mediaSources.isEmpty()) {
        variantId = movie.mediaSources.first().id;
    } else {
        QVariantMap vRes = co_await m_registry->callSource(
            m_sourceId, QStringLiteral("variants"), { { QStringLiteral("itemId"), movie.id } });
        const QVariantList variants = vRes.value(QStringLiteral("variants")).toList();
        if (!variants.isEmpty())
            variantId = variants.first().toMap().value(QStringLiteral("id")).toString();
    }
    if (variantId.isEmpty())
        variantId = movie.id;

    qint64 sourceBitrate = 0;
    for (const MediaSourceInfo& source : movie.mediaSources)
        sourceBitrate = std::max<qint64>(sourceBitrate, source.bitRate);

    const bool shouldTranscode
        = forceTranscode || (m_bitrateOverride > 0 && sourceBitrate > 0 && m_bitrateOverride < sourceBitrate);

    QVariantMap args;
    args.insert(QStringLiteral("itemId"), movie.id);
    args.insert(QStringLiteral("variantId"), variantId);
    args.insert(QStringLiteral("forceTranscode"), shouldTranscode);
    args.insert(QStringLiteral("positionTicks"), movie.resumeTicks);

    const qint64 maxBitrate = m_bitrateOverride > 0 ? m_bitrateOverride : 120'000'000;
    args.insert(QStringLiteral("maxBitrate"), maxBitrate);
    if (m_heightOverride > 0)
        args.insert(QStringLiteral("maxHeight"), m_heightOverride);

    const QJsonObject profile
        = PlaybackNegotiation::buildDeviceProfile(maxBitrate, m_heightOverride, m_videoCodecs, m_restrictVideoCodecs);
    args.insert(QStringLiteral("deviceProfile"), profile.toVariantMap());

    QVariantMap result = co_await m_registry->callSource(m_sourceId, QStringLiteral("resolve"), args);

    PlaybackSession session;
    session.itemId = movie.id;
    session.title = movie.title;
    session.itemType = movie.itemType;
    const QVariantMap video = result.value(QStringLiteral("video")).toMap();
    session.url = video.value(QStringLiteral("url")).toString();
    session.mediaSourceId = result.value(QStringLiteral("variantId")).toString();
    session.playSessionId = result.value(QStringLiteral("playSessionId")).toString();
    session.playMethod = result.value(QStringLiteral("playMethod")).toString();
    if (session.playMethod.isEmpty())
        session.playMethod = QStringLiteral("DirectPlay");
    session.startTimeTicks = movie.resumeTicks;
    session.runtimeTicks = movie.runtimeTicks;

    const QVariantList streams = result.value(QStringLiteral("streams")).toList();
    for (const auto& sVal : streams) {
        const QVariantMap s = sVal.toMap();
        MediaStreamInfo info;
        info.index = s.value(QStringLiteral("index")).toInt();
        info.type = s.value(QStringLiteral("type")).toString();
        info.codec = s.value(QStringLiteral("codec")).toString();
        info.language = s.value(QStringLiteral("language")).toString();
        info.title = s.value(QStringLiteral("title")).toString();
        info.displayTitle = info.title;
        info.width = s.value(QStringLiteral("width")).toInt();
        info.height = s.value(QStringLiteral("height")).toInt();
        info.channels = s.value(QStringLiteral("channels")).toInt();
        info.bitRate = s.value(QStringLiteral("bitrate")).toInt();
        info.videoRange = s.value(QStringLiteral("range")).toString();
        info.videoRangeType = info.videoRange;
        info.isDefault = s.value(QStringLiteral("default")).toBool();
        info.isForced = s.value(QStringLiteral("forced")).toBool();
        info.isExternal = s.value(QStringLiteral("external")).toBool();
        session.mediaStreams.append(std::move(info));
    }

    try {
        session.segments = co_await fetchMediaSegments(movie.id);
    } catch (...) {
    }

    session.trickplay.width = 320;
    co_return session;
}

QCoro::Task<std::vector<MediaSegment>> JellyfinJsAdapter::fetchMediaSegments(QString itemId)
{
    if (itemId.isEmpty() || m_sourceId.isEmpty())
        co_return {};
    QVariantMap res = co_await m_registry->callSource(
        m_sourceId, QStringLiteral("segments"), { { QStringLiteral("itemId"), itemId } });
    std::vector<MediaSegment> segments;
    const QVariantList list = res.value(QStringLiteral("segments")).toList();
    segments.reserve(list.size());
    for (const auto& val : list) {
        const QVariantMap map = val.toMap();
        MediaSegment seg;
        seg.id = map.value(QStringLiteral("Id")).toString();
        seg.type = map.value(QStringLiteral("Type")).toString();
        seg.startTicks = map.value(QStringLiteral("StartTicks")).toLongLong();
        seg.endTicks = map.value(QStringLiteral("EndTicks")).toLongLong();
        segments.push_back(std::move(seg));
    }
    co_return segments;
}

QByteArray JellyfinJsAdapter::mediaRequestHeaders() const
{
    if (m_sessionToken.isEmpty())
        return {};
    return "X-Emby-Token: " + m_sessionToken.toUtf8() + "\n";
}

QUrl JellyfinJsAdapter::mediaOrigin() const
{
    return QUrl(m_serverUrl);
}

QString JellyfinJsAdapter::trickplayTileUrl(const QString& itemId, int width, int tileIndex) const
{
    if (m_serverUrl.isEmpty() || itemId.isEmpty() || width <= 0 || tileIndex < 0)
        return {};
    return serverUrlWithPath(m_serverUrl,
        { QStringLiteral("Videos"), itemId, QStringLiteral("Trickplay"), QString::number(width),
            QStringLiteral("%1.jpg").arg(tileIndex) })
        .toString(QUrl::FullyEncoded);
}

QCoro::Task<void> JellyfinJsAdapter::reportPlaybackStart(
    PlaybackSession session, double playbackRate, int volume, bool muted)
{
    if (m_sourceId.isEmpty())
        co_return;
    co_await m_registry->callSource(m_sourceId, QStringLiteral("report"),
        { { QStringLiteral("event"), QStringLiteral("start") }, { QStringLiteral("itemId"), session.itemId },
            { QStringLiteral("variantId"), session.mediaSourceId },
            { QStringLiteral("playSessionId"), session.playSessionId },
            { QStringLiteral("positionTicks"), QString::number(session.startTimeTicks) },
            { QStringLiteral("rate"), playbackRate }, { QStringLiteral("volume"), volume },
            { QStringLiteral("muted"), muted }, { QStringLiteral("playMethod"), session.playMethod } });
}

QCoro::Task<void> JellyfinJsAdapter::reportPlaybackProgress(
    PlaybackSession session, qint64 positionTicks, bool paused, double playbackRate, int volume, bool muted)
{
    if (m_sourceId.isEmpty())
        co_return;
    co_await m_registry->callSource(m_sourceId, QStringLiteral("report"),
        { { QStringLiteral("event"), QStringLiteral("progress") }, { QStringLiteral("itemId"), session.itemId },
            { QStringLiteral("variantId"), session.mediaSourceId },
            { QStringLiteral("playSessionId"), session.playSessionId },
            { QStringLiteral("positionTicks"), QString::number(positionTicks) }, { QStringLiteral("paused"), paused },
            { QStringLiteral("rate"), playbackRate }, { QStringLiteral("volume"), volume },
            { QStringLiteral("muted"), muted }, { QStringLiteral("playMethod"), session.playMethod } });
}

QCoro::Task<void> JellyfinJsAdapter::reportPlaybackStopped(
    PlaybackSession session, qint64 positionTicks, bool, double playbackRate)
{
    if (m_sourceId.isEmpty())
        co_return;
    co_await m_registry->callSource(m_sourceId, QStringLiteral("report"),
        { { QStringLiteral("event"), QStringLiteral("stop") }, { QStringLiteral("itemId"), session.itemId },
            { QStringLiteral("variantId"), session.mediaSourceId },
            { QStringLiteral("playSessionId"), session.playSessionId },
            { QStringLiteral("positionTicks"), QString::number(positionTicks) },
            { QStringLiteral("rate"), playbackRate } });
}

} // namespace JellyfinNative

#include "JellyfinJsAdapter.moc"
