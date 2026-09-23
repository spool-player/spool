#include "PortableProvider.h"

#include "ProviderRegistry.h"

#include <QUrl>

#include <algorithm>

namespace Spool {

namespace {
    QString fill(QString pattern, std::initializer_list<std::pair<const char *, QString>> values)
    {
        for (const auto& [name, value] : values)
            pattern.replace(QLatin1Char('{') + QLatin1String(name) + QLatin1Char('}'),
                QString::fromLatin1(QUrl::toPercentEncoding(value)));
        return pattern;
    }

    QByteArray headerLines(const QVariantMap& headers)
    {
        QByteArray lines;
        for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
            const QByteArray value = it.value().toString().toUtf8();
            if (!value.contains('\n') && !value.contains('\r') && !it.key().contains(QLatin1Char('\n')))
                lines += it.key().toUtf8() + ": " + value + '\n';
        }
        return lines;
    }

    MediaStreamInfo streamFrom(const QVariantMap& s)
    {
        MediaStreamInfo info;
        info.index = s.value(QStringLiteral("index"), -1).toInt();
        info.type = s.value(QStringLiteral("type")).toString();
        info.codec = s.value(QStringLiteral("codec")).toString();
        info.profile = s.value(QStringLiteral("profile")).toString();
        info.language = s.value(QStringLiteral("language")).toString();
        info.title = s.value(QStringLiteral("title")).toString();
        info.displayTitle = info.title;
        info.width = s.value(QStringLiteral("width")).toInt();
        info.height = s.value(QStringLiteral("height")).toInt();
        info.frameRate = s.value(QStringLiteral("frameRate")).toDouble();
        info.channels = s.value(QStringLiteral("channels")).toInt();
        info.sampleRate = s.value(QStringLiteral("sampleRate")).toInt();
        info.bitRate = s.value(QStringLiteral("bitrate")).toInt();
        info.bitDepth = s.value(QStringLiteral("bitDepth")).toInt();
        info.videoRange = s.value(QStringLiteral("range")).toString();
        info.videoRangeType = s.value(QStringLiteral("rangeType"), info.videoRange).toString();
        info.isDefault = s.value(QStringLiteral("default")).toBool();
        info.isForced = s.value(QStringLiteral("forced")).toBool();
        info.isExternal = s.value(QStringLiteral("external")).toBool();
        info.isInterlaced = s.value(QStringLiteral("interlaced")).toBool();
        return info;
    }

    std::vector<MediaSegment> segmentsFrom(const QVariantList& rows)
    {
        std::vector<MediaSegment> segments;
        for (const QVariant& value : rows) {
            const QVariantMap row = value.toMap();
            segments.push_back({ row.value(QStringLiteral("id")).toString(),
                row.value(QStringLiteral("type")).toString(), row.value(QStringLiteral("startTicks")).toLongLong(),
                row.value(QStringLiteral("endTicks")).toLongLong() });
        }
        return segments;
    }

    // Every operation but describe is optional: one a provider leaves out
    // has nothing to list, which is not an error worth reporting.
    template <typename T> QCoro::Task<T> orEmpty(QCoro::Task<T> task)
    {
        try {
            co_return co_await std::move(task);
        } catch (const std::exception& error) {
            if (QByteArray(error.what()) != "unsupported_operation")
                throw;
        }
        co_return T {};
    }

    QString cursorFor(int startIndex)
    {
        return startIndex > 0 ? QString::number(startIndex) : QString();
    }
} // namespace

class PortableProvider::Playback final : public PlaybackSource {
public:
    explicit Playback(PortableProvider *owner)
        : PlaybackSource(owner)
        , m_owner(owner)
    {
    }

    QByteArray mediaRequestHeaders() const override
    {
        return m_headers;
    }
    QUrl mediaOrigin() const override
    {
        return m_origin;
    }
    int playbackParallelRequests() const override
    {
        return 2;
    }
    bool signedIn() const override
    {
        return true;
    }
    QString trickplayTileUrl(const QString& itemId, int width, int tileIndex) const override
    {
        if (m_owner->m_trickplayTemplate.isEmpty() || itemId.isEmpty() || width <= 0 || tileIndex < 0)
            return {};
        return fill(m_owner->m_trickplayTemplate,
            { { "itemId", itemId }, { "width", QString::number(width) }, { "index", QString::number(tileIndex) },
                { "variantId", m_variantId } });
    }

    QCoro::Task<PlaybackSession> resolvePlayback(MovieItem item, bool forceTranscode) override
    {
        QVariantMap args = m_owner->m_playbackContext;
        args.insert(QStringLiteral("itemId"), item.id);
        if (!item.mediaSources.isEmpty())
            args.insert(QStringLiteral("variantId"), item.mediaSources.constFirst().id);
        args.insert(QStringLiteral("positionTicks"), QString::number(item.resumeTicks));
        args.insert(QStringLiteral("forceTranscode"), forceTranscode);
        QVariantMap result = co_await m_owner->call(QStringLiteral("resolve"), args);
        // A provider that offers a choice (releases, files, mirrors) hands
        // the viewer its own picker and resolves again with the answer.
        if (result.contains(QStringLiteral("pick"))) {
            const QVariantMap choice = co_await m_owner->m_registry->pick(
                m_owner->m_accountId, result.value(QStringLiteral("pick")).toMap());
            if (choice.isEmpty())
                throw std::runtime_error("playback_cancelled");
            args.insert(choice);
            result = co_await m_owner->call(QStringLiteral("resolve"), args);
        }

        PlaybackSession session;
        session.itemId = item.id;
        session.title = item.title;
        session.itemType = item.itemType;
        session.url = result.value(QStringLiteral("url")).toString();
        if (session.url.isEmpty())
            throw std::runtime_error("playback_unavailable");
        session.mediaSourceId = result.value(QStringLiteral("variantId")).toString();
        session.playSessionId = result.value(QStringLiteral("playSessionId")).toString();
        session.playMethod = result.value(QStringLiteral("playMethod"), QStringLiteral("DirectPlay")).toString();
        session.container = result.value(QStringLiteral("container")).toString();
        session.startTimeTicks = item.resumeTicks;
        session.runtimeTicks = item.runtimeTicks;
        for (const QVariant& stream : result.value(QStringLiteral("streams")).toList())
            session.mediaStreams.append(streamFrom(stream.toMap()));
        session.segments = segmentsFrom(result.value(QStringLiteral("segments")).toList());
        const QVariantMap trickplay = result.value(QStringLiteral("trickplay")).toMap();
        session.trickplay = { trickplay.value(QStringLiteral("width")).toInt(),
            trickplay.value(QStringLiteral("height")).toInt(), trickplay.value(QStringLiteral("columns")).toInt(),
            trickplay.value(QStringLiteral("rows")).toInt(), trickplay.value(QStringLiteral("count")).toInt(),
            trickplay.value(QStringLiteral("intervalMs")).toInt(), 0 };

        const QByteArray headers = headerLines(result.value(QStringLiteral("headers")).toMap());
        const QUrl origin = QUrl(session.url).adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);
        m_variantId = session.mediaSourceId;
        if (headers != m_headers || origin != m_origin) {
            m_headers = headers;
            m_origin = origin;
            emit credentialsChanged();
        }
        co_return session;
    }

    QCoro::Task<std::vector<MediaSegment>> fetchMediaSegments(QString itemId) override
    {
        const QVariantMap result
            = co_await orEmpty(m_owner->call(QStringLiteral("segments"), { { QStringLiteral("itemId"), itemId } }));
        co_return segmentsFrom(result.value(QStringLiteral("segments")).toList());
    }

    QCoro::Task<std::vector<MovieItem>> fetchSeriesEpisodes(QString seriesId) override
    {
        return m_owner->fetchEpisodes(std::move(seriesId));
    }

    QCoro::Task<void> reportPlaybackStart(PlaybackSession session, double rate, int volume, bool muted) override
    {
        return report(QStringLiteral("start"), session, session.startTimeTicks,
            { { QStringLiteral("rate"), rate }, { QStringLiteral("volume"), volume },
                { QStringLiteral("muted"), muted } });
    }
    QCoro::Task<void> reportPlaybackProgress(
        PlaybackSession session, qint64 positionTicks, bool paused, double rate, int volume, bool muted) override
    {
        return report(QStringLiteral("progress"), session, positionTicks,
            { { QStringLiteral("paused"), paused }, { QStringLiteral("rate"), rate },
                { QStringLiteral("volume"), volume }, { QStringLiteral("muted"), muted } });
    }
    QCoro::Task<void> reportPlaybackStopped(
        PlaybackSession session, qint64 positionTicks, bool failed, double rate) override
    {
        return report(QStringLiteral("stop"), session, positionTicks,
            { { QStringLiteral("failed"), failed }, { QStringLiteral("rate"), rate } });
    }

private:
    QCoro::Task<void> report(QString event, PlaybackSession session, qint64 positionTicks, QVariantMap args)
    {
        if (!m_owner->m_capabilities.testFlag(PlaybackReporting))
            co_return;
        args.insert(QStringLiteral("event"), event);
        args.insert(QStringLiteral("itemId"), session.itemId);
        args.insert(QStringLiteral("variantId"), session.mediaSourceId);
        args.insert(QStringLiteral("playSessionId"), session.playSessionId);
        args.insert(QStringLiteral("playMethod"), session.playMethod);
        args.insert(QStringLiteral("positionTicks"), QString::number(positionTicks));
        args.insert(QStringLiteral("audioStreamIndex"), session.audioStreamIndex);
        args.insert(QStringLiteral("subtitleStreamIndex"), session.subtitleStreamIndex);
        co_await m_owner->call(QStringLiteral("report"), args);
    }

    PortableProvider *m_owner;
    QByteArray m_headers;
    QUrl m_origin;
    QString m_variantId;
};

PortableProvider::PortableProvider(ProviderRegistry *registry, QString accountId, QString label,
    Capabilities capabilities, const QVariantMap& description, QObject *parent)
    : Provider(parent)
    , m_registry(registry)
    , m_accountId(std::move(accountId))
    , m_label(std::move(label))
    , m_capabilities(capabilities)
    , m_artworkTemplate(description.value(QStringLiteral("artwork")).toString())
    , m_trickplayTemplate(description.value(QStringLiteral("trickplay")).toString())
    , m_playback(new Playback(this))
{
}

PortableProvider::~PortableProvider() = default;

PlaybackSource *PortableProvider::playback()
{
    return m_playback;
}

QCoro::Task<QVariantMap> PortableProvider::call(QString operation, QVariantMap arguments)
{
    return m_registry->callSource(m_accountId, std::move(operation), std::move(arguments));
}

QCoro::Task<std::vector<MovieItem>> PortableProvider::list(
    QString operation, QVariantMap arguments, int limit, QString scope)
{
    limit = std::clamp(limit, 1, 100);
    arguments.insert(QStringLiteral("limit"), limit);
    ProviderMediaPage page = co_await orEmpty(
        m_registry->callSourceMediaPage(m_accountId, std::move(operation), arguments, limit, std::move(scope)));
    co_return std::move(page.items);
}

QCoro::Task<PagedMovieItems> PortableProvider::fetchBrowsePage(
    BrowseDescriptor descriptor, int startIndex, int limit, QVariantMap queryOptions)
{
    startIndex = std::max(0, startIndex);
    limit = std::clamp(limit, 1, 100);
    QString operation = QStringLiteral("browse");
    QVariantMap args { { QStringLiteral("cursor"), cursorFor(startIndex) }, { QStringLiteral("limit"), limit } };
    switch (descriptor.kind) {
    case BrowseKind::Library:
    case BrowseKind::Genre:
    case BrowseKind::Studio:
        if (!descriptor.id.isEmpty() && descriptor.kind == BrowseKind::Library)
            args.insert(QStringLiteral("parentId"), descriptor.id);
        args.insert(QStringLiteral("collectionType"), descriptor.collectionType);
        if (descriptor.kind == BrowseKind::Genre)
            args.insert(QStringLiteral("genre"), descriptor.name);
        if (descriptor.kind == BrowseKind::Studio)
            args.insert(QStringLiteral("studio"), descriptor.name);
        for (const char *key : { "sortBy", "sortOrder", "filters" }) {
            const QString name = QLatin1String(key);
            if (queryOptions.contains(name))
                args.insert(name, queryOptions.value(name));
        }
        break;
    case BrowseKind::FolderChildren:
    case BrowseKind::BoxSet:
    case BrowseKind::Playlist:
    case BrowseKind::ArtistAlbums:
        args.insert(QStringLiteral("parentId"), descriptor.id);
        args.insert(QStringLiteral("recursive"), false);
        break;
    case BrowseKind::SeriesSeasons:
        operation = QStringLiteral("seasons");
        args.insert(QStringLiteral("seriesId"), descriptor.seriesId.isEmpty() ? descriptor.id : descriptor.seriesId);
        break;
    case BrowseKind::SeasonEpisodes:
        operation = QStringLiteral("episodes");
        args.insert(QStringLiteral("seriesId"), descriptor.seriesId);
        args.insert(QStringLiteral("seasonId"), descriptor.seasonId);
        break;
    case BrowseKind::Person:
        operation = QStringLiteral("personItems");
        args.insert(QStringLiteral("personId"), descriptor.id);
        break;
    case BrowseKind::None:
        co_return PagedMovieItems { {}, 0, startIndex, limit };
    }

    ProviderMediaPage page = co_await m_registry->callSourceMediaPage(m_accountId, operation, args, limit);
    const int count = static_cast<int>(page.items.size());
    // Core pages by offset; a provider whose cursor is not an offset still
    // pages correctly as long as it reports exhaustion.
    const int total = page.total ? static_cast<int>(*page.total) : startIndex + count + (page.exhausted ? 0 : 1);
    co_return PagedMovieItems { std::move(page.items), total, startIndex, limit };
}

QCoro::Task<MovieItem> PortableProvider::fetchItemDetails(QString itemId)
{
    return m_registry->callSourceItem(m_accountId, QStringLiteral("details"), { { QStringLiteral("itemId"), itemId } });
}

QCoro::Task<std::vector<MovieItem>> PortableProvider::fetchSeasons(QString seriesId)
{
    auto seasons = co_await list(QStringLiteral("seasons"), { { QStringLiteral("seriesId"), seriesId } }, 100);
    for (MovieItem& season : seasons) {
        if (season.seriesId.isEmpty())
            season.seriesId = seriesId;
    }
    co_return seasons;
}

QCoro::Task<std::vector<MovieItem>> PortableProvider::fetchEpisodes(QString seriesId, QString seasonId)
{
    return list(QStringLiteral("episodes"),
        { { QStringLiteral("seriesId"), seriesId }, { QStringLiteral("seasonId"), seasonId } }, 100);
}

QCoro::Task<std::vector<MovieItem>> PortableProvider::fetchResumeItems(int limit)
{
    return list(QStringLiteral("resume"), {}, limit);
}

QCoro::Task<std::vector<MovieItem>> PortableProvider::fetchNextUpEpisodes(int limit)
{
    return list(QStringLiteral("nextUp"), {}, limit);
}

QCoro::Task<std::vector<MovieItem>> PortableProvider::fetchLatestItems(QString parentId, int limit)
{
    return list(QStringLiteral("latest"), { { QStringLiteral("parentId"), parentId } }, limit);
}

QCoro::Task<std::vector<MovieItem>> PortableProvider::fetchSimilarItems(QString itemId, int limit)
{
    return list(QStringLiteral("similar"), { { QStringLiteral("itemId"), itemId } }, limit);
}

QCoro::Task<PersonCredits> PortableProvider::fetchItemsByPerson(QString personId, int maximumItems)
{
    PersonCredits credits;
    credits.items = co_await list(
        QStringLiteral("personItems"), { { QStringLiteral("personId"), personId } }, std::min(maximumItems, 100));
    co_return credits;
}

QCoro::Task<std::vector<LibraryItem>> PortableProvider::fetchLibraries()
{
    const QVariantMap result = co_await call(QStringLiteral("libraries"));
    std::vector<LibraryItem> libraries;
    for (const QVariant& value : result.value(QStringLiteral("items")).toList()) {
        const QVariantMap row = value.toMap();
        libraries.push_back({ row.value(QStringLiteral("id")).toString(), row.value(QStringLiteral("title")).toString(),
            row.value(QStringLiteral("collectionType")).toString(),
            row.value(QStringLiteral("posterTag")).toString() });
    }
    co_return libraries;
}

QCoro::Task<QVariantMap> PortableProvider::fetchLibraryFilterOptions(QString libraryId, QString collectionType)
{
    return orEmpty(call(QStringLiteral("filterOptions"),
        { { QStringLiteral("parentId"), libraryId }, { QStringLiteral("collectionType"), collectionType } }));
}

QCoro::Task<std::vector<MovieItem>> PortableProvider::fetchItemsByIds(QStringList itemIds)
{
    if (itemIds.isEmpty())
        co_return {};
    co_return co_await list(QStringLiteral("items"), { { QStringLiteral("ids"), itemIds } }, int(itemIds.size()));
}

QCoro::Task<std::vector<MovieItem>> PortableProvider::searchItems(QString searchTerm, int limit)
{
    // One search per account in flight: SourceHub cancels this scope when
    // the query moves on, so stale searches stop holding operation slots.
    return list(QStringLiteral("search"), { { QStringLiteral("query"), searchTerm } }, limit, QStringLiteral("search"));
}

QCoro::Task<std::vector<MovieItem>> PortableProvider::fetchSearchSuggestions(int limit)
{
    return list(QStringLiteral("resume"), {}, limit);
}

QCoro::Task<void> PortableProvider::setItemFavorite(QString itemId, bool favorite)
{
    co_await call(
        QStringLiteral("favorite"), { { QStringLiteral("itemId"), itemId }, { QStringLiteral("value"), favorite } });
}

QCoro::Task<void> PortableProvider::setItemPlayed(QString itemId, bool played)
{
    co_await call(
        QStringLiteral("played"), { { QStringLiteral("itemId"), itemId }, { QStringLiteral("value"), played } });
}

QCoro::Task<void> PortableProvider::setItemPlaybackPosition(QString itemId, qint64 positionTicks)
{
    co_await call(QStringLiteral("progress"),
        { { QStringLiteral("itemId"), itemId }, { QStringLiteral("positionTicks"), QString::number(positionTicks) } });
}

QString PortableProvider::imageUrl(const ImageRequest& request) const
{
    if (request.tag.isEmpty())
        return {};
    // A provider with no template sends absolute image URLs as the tag.
    if (m_artworkTemplate.isEmpty())
        return request.tag.startsWith(QStringLiteral("https://")) || request.tag.startsWith(QStringLiteral("http://"))
            ? request.tag
            : QString();
    const int width = request.fillWidth > 0 ? request.fillWidth : request.maxWidth;
    return fill(m_artworkTemplate,
        { { "itemId", request.itemId }, { "type", request.imageType }, { "tag", request.tag },
            { "width", QString::number(width) }, { "height", QString::number(request.fillHeight) },
            { "quality", QString::number(request.quality) }, { "format", request.format } });
}

} // namespace Spool
