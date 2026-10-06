#include "PortableProvider.h"

#include "ProviderLogging.h"
#include "ProviderRegistry.h"

#include <QCoroFuture>
#include <QPointer>
#include <QPromise>
#include <QScopeGuard>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <iterator>
#include <utility>

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
            const QByteArray name = it.key().toLatin1();
            if (!name.isEmpty() && name.size() <= 128
                && std::all_of(name.begin(), name.end(),
                    [](unsigned char c) {
                        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                            || QByteArrayView("!#$%&'*+-.^_`|~").contains(c);
                    })
                && !value.contains('\n') && !value.contains('\r'))
                lines += name + ": " + value + '\n';
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
        info.deliveryUrl = s.value(QStringLiteral("url")).toString();
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

} // namespace

class PortableProvider::Playback final : public PlaybackSource {
public:
    explicit Playback(PortableProvider *owner)
        : PlaybackSource(owner)
        , m_owner(owner)
    {
        connect(owner->m_registry, &ProviderRegistry::extensionsChanged, this, [this](const QString& account) {
            if (account == m_owner->m_accountId) {
                ++m_queueSupportGeneration;
                m_reportedQueueRevision.clear();
            }
        });
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
        return m_owner->m_playbackContext.value(QStringLiteral("parallelRequests"), 2).toInt();
    }
    bool signedIn() const override
    {
        return true;
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
        bool validOrigin = false;
        session.timelineOriginTicks = result.value(QStringLiteral("timelineOriginTicks"), QStringLiteral("0"))
                                          .toString()
                                          .toLongLong(&validOrigin);
        if (!validOrigin || session.timelineOriginTicks < 0 || session.timelineOriginTicks > session.startTimeTicks)
            throw std::runtime_error("invalid_playback_timeline");
        session.runtimeTicks = item.runtimeTicks;
        const QVariantMap source = result.value(QStringLiteral("source")).toMap();
        session.sourceBitrate = source.value(QStringLiteral("bitrate")).toLongLong();
        session.sourceWidth = source.value(QStringLiteral("width")).toInt();
        session.sourceHeight = source.value(QStringLiteral("height")).toInt();
        // mpv lists a file's own tracks first and subtitle files after them,
        // in the order they are added, so the streams are kept in that order.
        // A subtitle file mpv cannot fetch is left out rather than shifting
        // every track after it; one on another origin would take this
        // stream's credentials with it.
        const QUrl streamOrigin
            = QUrl(session.url).adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);
        QList<MediaStreamInfo> externalSubtitles;
        for (const QVariant& stream : result.value(QStringLiteral("streams")).toList()) {
            MediaStreamInfo info = streamFrom(stream.toMap());
            if (!info.isExternal) {
                session.mediaStreams.append(info);
                continue;
            }
            const QUrl url(info.deliveryUrl, QUrl::StrictMode);
            if (info.type == QStringLiteral("Subtitle") && url.isValid()
                && url.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment) == streamOrigin)
                externalSubtitles.append(info);
        }
        session.mediaStreams.append(externalSubtitles);
        session.segments = segmentsFrom(result.value(QStringLiteral("segments")).toList());
        const QVariantMap trickplay = result.value(QStringLiteral("trickplay")).toMap();
        const bool previewsEnabled = m_owner->m_playbackContext.value(QStringLiteral("videoPreviews"), true).toBool();
        if (providerLogEnabled(ProviderLogLevel::Trace)) {
            const QString format = trickplay.value(QStringLiteral("format")).toString();
            const bool supported
                = format.isEmpty() || format == QLatin1String("sprites") || format == QLatin1String("bif");
            writeProviderLog(ProviderLogLevel::Trace,
                QStringLiteral("preview metadata item=%1 enabled=%2 availability=%3 format=%4")
                    .arg(item.id)
                    .arg(previewsEnabled)
                    .arg(trickplay.isEmpty() ? QStringLiteral("missing")
                            : supported      ? QStringLiteral("supported")
                                             : QStringLiteral("unsupported"))
                    .arg(format.isEmpty() ? QStringLiteral("sprites")
                            : supported   ? format
                                          : QStringLiteral("unknown")));
        }
        session.trickplay.width = trickplay.value(QStringLiteral("width")).toInt();
        session.trickplay.height = trickplay.value(QStringLiteral("height")).toInt();
        session.trickplay.tileWidth = trickplay.value(QStringLiteral("columns")).toInt();
        session.trickplay.tileHeight = trickplay.value(QStringLiteral("rows")).toInt();
        session.trickplay.thumbnailCount = trickplay.value(QStringLiteral("count")).toInt();
        session.trickplay.intervalMs = trickplay.value(QStringLiteral("intervalMs")).toInt();
        session.trickplay.urlTemplate = trickplay.value(QStringLiteral("urlTemplate")).toString();
        session.trickplay.format = trickplay.value(QStringLiteral("format")).toString();
        session.trickplay.url = trickplay.value(QStringLiteral("url")).toString();
        session.trickplay.headers = headerLines(trickplay.value(QStringLiteral("headers")).toMap());

        const QByteArray headers = headerLines(result.value(QStringLiteral("headers")).toMap());
        const QUrl origin = QUrl(session.url).adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);
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
        QString queueRevision;
        const quint64 supportGeneration = m_queueSupportGeneration;
        if (event != QStringLiteral("stop")
            && m_owner->m_registry->extensionVersion(
                   m_owner->m_accountId, QStringLiteral("spool.playback-queue-reporting"))
                == 1) {
            queueRevision = m_owner->m_queueSnapshot.value(QStringLiteral("revision")).toString();
            if (!queueRevision.isEmpty()
                && (event == QStringLiteral("start") || queueRevision != m_reportedQueueRevision))
                args.insert(QStringLiteral("queue"), m_owner->m_queueSnapshot);
            if (m_owner->m_queueIndex >= 0)
                args.insert(QStringLiteral("queueIndex"), m_owner->m_queueIndex);
        }
        co_await m_owner->call(QStringLiteral("report"), args);
        if (!queueRevision.isEmpty() && supportGeneration == m_queueSupportGeneration)
            m_reportedQueueRevision = queueRevision;
    }

    PortableProvider *m_owner;
    QByteArray m_headers;
    QUrl m_origin;
    QString m_reportedQueueRevision;
    quint64 m_queueSupportGeneration = 0;
};

void PortableProvider::setPlaybackContext(QVariantMap context)
{
    const int previous = m_playback->playbackParallelRequests();
    m_playbackContext = std::move(context);
    if (previous != m_playback->playbackParallelRequests())
        emit m_playback->playbackNetworkProfileChanged();
}

void PortableProvider::setPlaybackQueueContext(QVariantMap snapshot, int index)
{
    m_queueSnapshot = std::move(snapshot);
    m_queueIndex = index;
}

void PortableProvider::setExtensionSpeedTest(bool enabled)
{
    const Capabilities before = m_capabilities;
    m_capabilities.setFlag(SpeedTest, m_legacySpeedTest || enabled);
    if (before != m_capabilities)
        emit capabilitiesChanged();
}

PortableProvider::PortableProvider(ProviderRegistry *registry, QString accountId, QString label,
    Capabilities capabilities, const QVariantMap& description, QObject *parent)
    : Provider(parent)
    , m_registry(registry)
    , m_accountId(std::move(accountId))
    , m_label(std::move(label))
    , m_capabilities(capabilities)
    , m_legacySpeedTest(capabilities.testFlag(SpeedTest))
    , m_artworkTemplate(description.value(QStringLiteral("artwork")).toString())
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

QCoro::Task<DownloadPlan> PortableProvider::negotiateDownload(DownloadRequest request, QString scope)
{
    QPointer<PortableProvider> guard(this);
    QPointer<ProviderRegistry> registry(m_registry);
    const QString accountId = m_accountId;
    bool cancelled = false;
    const auto cancellation = connect(registry, &ProviderRegistry::sourceScopeCancelled, registry,
        [&cancelled, accountId, scope](const QString& sourceId, const QString& cancelledScope) {
            if (sourceId == accountId && (cancelledScope.isEmpty() || cancelledScope == scope))
                cancelled = true;
        });
    const auto disconnectCancellation = qScopeGuard([cancellation] { QObject::disconnect(cancellation); });
    QVariantMap args { { QStringLiteral("itemId"), request.itemId },
        { QStringLiteral("mode"), request.transcode ? QStringLiteral("transcoded") : QStringLiteral("original") } };
    if (!request.variantId.isEmpty())
        args.insert(QStringLiteral("variantId"), request.variantId);
    if (request.maxBitrate > 0)
        args.insert(QStringLiteral("maxBitrate"), request.maxBitrate);
    if (request.maxHeight > 0)
        args.insert(QStringLiteral("maxHeight"), request.maxHeight);
    QVariantMap result = co_await registry->callSource(accountId, QStringLiteral("download"), args, scope);
    if (!guard || !registry)
        throw std::runtime_error("source_unavailable");
    if (result.contains(QStringLiteral("pick"))) {
        if (cancelled)
            throw std::runtime_error("download_cancelled");
        const QVariantMap choice
            = co_await registry->pick(accountId, result.value(QStringLiteral("pick")).toMap(), scope);
        if (!guard || !registry)
            throw std::runtime_error("source_unavailable");
        if (cancelled || choice.isEmpty())
            throw std::runtime_error("download_cancelled");
        args.insert(choice);
        // A picker cannot replace the native-controlled item or quality request.
        args.insert(QStringLiteral("itemId"), request.itemId);
        args.insert(
            QStringLiteral("mode"), request.transcode ? QStringLiteral("transcoded") : QStringLiteral("original"));
        args.remove(QStringLiteral("maxBitrate"));
        args.remove(QStringLiteral("maxHeight"));
        if (request.maxBitrate > 0)
            args.insert(QStringLiteral("maxBitrate"), request.maxBitrate);
        if (request.maxHeight > 0)
            args.insert(QStringLiteral("maxHeight"), request.maxHeight);
        result = co_await registry->callSource(accountId, QStringLiteral("download"), args, scope);
    }
    co_return DownloadPlan { QUrl(result.value(QStringLiteral("url")).toString()),
        result.value(QStringLiteral("container")).toString().toLower(), result.value(QStringLiteral("headers")).toMap(),
        result.value(QStringLiteral("size"), -1).toLongLong(), result.value(QStringLiteral("cleanup")).toMap() };
}

QCoro::Task<void> PortableProvider::releaseDownload(QVariantMap cleanup)
{
    if (!cleanup.isEmpty())
        co_await call(QStringLiteral("downloadRelease"), { { QStringLiteral("cleanup"), cleanup } });
}

QCoro::Task<ProviderMediaPage> PortableProvider::listPage(
    QString operation, QVariantMap arguments, int limit, std::optional<QString> cursor, QString scope)
{
    arguments.insert(QStringLiteral("limit"), limit);
    if (cursor)
        arguments.insert(QStringLiteral("cursor"), *cursor);
    else
        arguments.remove(QStringLiteral("cursor"));
    try {
        co_return co_await m_registry->callSourceMediaPage(
            m_accountId, std::move(operation), std::move(arguments), limit, std::move(scope));
    } catch (const std::exception& error) {
        if (QByteArray(error.what()) != "unsupported_operation")
            throw;
    }
    co_return ProviderMediaPage { {}, {}, {}, true };
}

QCoro::Task<std::vector<MovieItem>> PortableProvider::list(
    QString operation, QVariantMap arguments, std::optional<int> limit, QString scope)
{
    std::vector<MovieItem> items;
    const QPointer<PortableProvider> guard(this);
    if (limit && *limit <= 0)
        co_return items;
    std::optional<QString> cursor;
    QSet<QString> seen;
    for (int pages = 0; pages < 256; ++pages) {
        if (!guard)
            throw std::runtime_error("source_unavailable");
        const int remaining = limit ? *limit - static_cast<int>(items.size()) : 10000 - static_cast<int>(items.size());
        ProviderMediaPage page = co_await listPage(operation, arguments, std::min(remaining, 100), cursor, scope);
        if (!page.exhausted) {
            if (!page.cursor || page.cursor->isEmpty() || seen.contains(*page.cursor))
                throw std::runtime_error("invalid_pagination");
            seen.insert(*page.cursor);
        }
        items.insert(
            items.end(), std::make_move_iterator(page.items.begin()), std::make_move_iterator(page.items.end()));
        if (page.exhausted || (limit && items.size() >= static_cast<size_t>(*limit)))
            co_return items;
        if (!limit && items.size() >= 10000)
            throw std::runtime_error("response_limit");
        cursor = std::move(page.cursor);
    }
    throw std::runtime_error("response_limit");
}

QCoro::Task<PagedMovieItems> PortableProvider::fetchBrowsePage(
    BrowseDescriptor descriptor, int startIndex, int limit, QVariantMap queryOptions, std::optional<QString> cursor)
{
    startIndex = std::max(0, startIndex);
    limit = std::clamp(limit, 1, 100);
    QString operation = QStringLiteral("browse");
    QVariantMap args;
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
        // Sorting travels on its own; every other option is one of the
        // viewer's filters (sdk/provider.d.ts BrowseFilters).
        for (const char *key : { "sortBy", "sortOrder" }) {
            if (const QString name = QLatin1String(key); queryOptions.contains(name))
                args.insert(name, queryOptions.take(name));
        }
        if (!queryOptions.isEmpty())
            args.insert(QStringLiteral("filters"), queryOptions);
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

    ProviderMediaPage page = co_await listPage(operation, std::move(args), limit, std::move(cursor));
    if (!page.exhausted && (!page.cursor || page.cursor->isEmpty()))
        throw std::runtime_error("invalid_pagination");
    const int count = static_cast<int>(page.items.size());
    const int total = page.total ? static_cast<int>(*page.total) : startIndex + count + (page.exhausted ? 0 : 1);
    co_return PagedMovieItems { std::move(page.items), total, startIndex, limit, std::move(page.cursor),
        page.exhausted };
}

QCoro::Task<MovieItem> PortableProvider::fetchItemDetails(QString itemId)
{
    return m_registry->callSourceItem(m_accountId, QStringLiteral("details"),
        { { QStringLiteral("itemId"), itemId },
            { QStringLiteral("videoPreviews"), m_playbackContext.value(QStringLiteral("videoPreviews"), true) } });
}

QCoro::Task<std::vector<MovieItem>> PortableProvider::fetchSeasons(QString seriesId)
{
    auto seasons = co_await list(QStringLiteral("seasons"), { { QStringLiteral("seriesId"), seriesId } }, std::nullopt);
    for (MovieItem& season : seasons) {
        if (season.seriesId.isEmpty())
            season.seriesId = seriesId;
    }
    co_return seasons;
}

QCoro::Task<std::vector<MovieItem>> PortableProvider::fetchEpisodes(QString seriesId, QString seasonId)
{
    return list(QStringLiteral("episodes"),
        { { QStringLiteral("seriesId"), seriesId }, { QStringLiteral("seasonId"), seasonId } }, std::nullopt);
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
    credits.items
        = co_await list(QStringLiteral("personItems"), { { QStringLiteral("personId"), personId } }, maximumItems);
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
    // Serialize invocations for this account, with two bounded batches in flight
    // inside each invocation. Source removal still cancels the registry operations.
    QPromise<void> turn;
    turn.start();
    const QFuture<void> previous = std::exchange(m_itemsTail, turn.future());
    const auto release = qScopeGuard([&turn] { turn.finish(); });
    const QPointer<PortableProvider> guard(this);
    if (previous.isValid())
        co_await qCoro(previous).result();
    QStringList unique = itemIds;
    unique.removeDuplicates();
    QHash<QString, MovieItem> found;
    for (qsizetype offset = 0; offset < unique.size(); offset += 100) {
        if (!guard)
            throw std::runtime_error("source_unavailable");
        std::vector<QCoro::Task<std::vector<MovieItem>>> pending;
        pending.reserve(2);
        for (qsizetype batch = offset; batch < std::min(offset + 100, unique.size()); batch += 50) {
            const QStringList ids = unique.mid(batch, std::min(qsizetype(50), unique.size() - batch));
            pending.push_back(list(QStringLiteral("items"), { { QStringLiteral("ids"), ids } }, int(ids.size())));
        }
        std::exception_ptr failure;
        for (auto& task : pending) {
            try {
                for (MovieItem& item : co_await std::move(task)) {
                    const QString id = item.id;
                    found.insert(id, std::move(item));
                }
            } catch (...) {
                if (!failure)
                    failure = std::current_exception();
            }
        }
        if (failure)
            std::rethrow_exception(failure);
    }
    std::vector<MovieItem> ordered;
    ordered.reserve(itemIds.size());
    for (const QString& id : itemIds) {
        const auto it = found.constFind(id);
        if (it != found.cend())
            ordered.push_back(*it);
    }
    co_return ordered;
}

QCoro::Task<std::vector<MovieItem>> PortableProvider::searchItems(QString searchTerm, int limit)
{
    // One search per account in flight: SourceHub cancels this scope when
    // the query moves on, so stale searches stop holding operation slots.
    return list(QStringLiteral("search"), { { QStringLiteral("query"), searchTerm } }, limit, QStringLiteral("search"));
}

QCoro::Task<std::vector<MovieItem>> PortableProvider::fetchSearchSuggestions(int limit)
{
    if (limit <= 0 || m_registry->extensionVersion(m_accountId, QStringLiteral("spool.suggestions")) != 1)
        co_return std::vector<MovieItem> {};
    co_return co_await list(QStringLiteral("suggestions"), {}, std::min(limit, 60), QStringLiteral("suggestions"));
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
