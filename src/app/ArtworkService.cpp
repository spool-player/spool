#include "ArtworkService.h"
#include "../common/TlsTrust.h"
#include "../platform/PlatformSettingsPolicy.h"
#include "ArtworkImageProvider.h"

#include <QBuffer>
#include <QDebug>
#include <QDir>
#include <QImageReader>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQueue>
#include <QQuickTextureFactory>
#include <QRunnable>
#include <QSet>
#include <QUrlQuery>

#include <chrono>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

namespace JellyfinNative {

namespace {

    constexpr int kRenderConcurrency = 6;
    // Each completed response becomes a texture upload in the next sync pass;
    // batches of 50+ posters land together, so cap completions per 16 ms tick
    // to keep individual frames short while a batch trickles in.
    constexpr int kMaxDeliveriesPerTick = 6;

    qint64 monotonicNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    QSize decodeSizeForRequest(const QSize& sourceSize, const QSize& requestedSize)
    {
        if (!sourceSize.isValid() || !requestedSize.isValid() || requestedSize.width() <= 0
            || requestedSize.height() <= 0)
            return {};
        return sourceSize.scaled(requestedSize, Qt::KeepAspectRatioByExpanding);
    }

#if defined(JELLYFIN_ARTWORK_ASPECT_DIAGNOSTICS)
    bool shouldLogAspectDiagnostic(const QSize& source, const QSize& requested, const QSize& decoded)
    {
        if (!source.isValid() || !requested.isValid() || !decoded.isValid() || source.width() <= 0
            || source.height() <= 0 || requested.width() <= 0 || requested.height() <= 0)
            return false;

        const double sourceAspect = static_cast<double>(source.width()) / source.height();
        const double requestedAspect = static_cast<double>(requested.width()) / requested.height();
        if (std::abs(sourceAspect - requestedAspect) <= 0.02)
            return false;

        if (requested.width() <= 4 || requested.height() <= 4)
            return true;

        const double horizontalOverscan = static_cast<double>(decoded.width()) / requested.width();
        const double verticalOverscan = static_cast<double>(decoded.height()) / requested.height();
        return std::max(horizontalOverscan, verticalOverscan) > 1.35;
    }
#endif

    QImage decodeArtwork(const QByteArray& bytes, const QSize& requestedSize, QString *error)
    {
        QBuffer buffer;
        buffer.setData(bytes);
        if (!buffer.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("Could not open artwork buffer");
            return {};
        }

        QImageReader reader(&buffer);
        reader.setAutoTransform(false);
        const QSize scaledSize = decodeSizeForRequest(reader.size(), requestedSize);
        if (scaledSize.isValid()) {
#if defined(JELLYFIN_ARTWORK_ASPECT_DIAGNOSTICS)
            if (shouldLogAspectDiagnostic(reader.size(), requestedSize, scaledSize)) {
                qWarning() << "artwork: aspect-preserving decode"
                           << "source=" << reader.size() << "requested=" << requestedSize << "decode=" << scaledSize;
            }
#endif
            reader.setScaledSize(scaledSize);
        }
        QImage image = reader.read();
        if (image.isNull())
            *error = reader.errorString();
        return image;
    }

    QString cacheKeyForUrl(const QUrl& url)
    {
        return url.toString(QUrl::FullyEncoded);
    }

} // namespace

class ArtworkFetchWorker final : public QObject {
public:
    ArtworkFetchWorker(
        QString cacheDirectory, qint64 networkCacheBytes, ArtworkService *service, TlsTrustController *tlsTrust)
        : m_cacheDirectory(std::move(cacheDirectory))
        , m_networkCacheBytes(networkCacheBytes)
        , m_service(service)
        , m_tlsTrust(tlsTrust)
    {
    }

    void fetchRender(int requestId, QUrl url)
    {
        ensureNetwork();
        if (!m_network || !url.isValid() || url.scheme().isEmpty()) {
            deliverRender(requestId, cacheKeyForUrl(url), {}, QStringLiteral("Invalid artwork URL"), false);
            return;
        }

        m_renderQueue.enqueue({ requestId, std::move(url), monotonicNs() });
        drainRender();
    }

    void cancelRender(int requestId)
    {
        for (auto it = m_renderQueue.begin(); it != m_renderQueue.end();) {
            if (it->requestId == requestId)
                it = m_renderQueue.erase(it);
            else
                ++it;
        }

        QNetworkReply *reply = m_renderReplies.take(requestId);
        if (reply)
            reply->abort();
        drainPrefetch();
    }

    void prefetch(QStringList urls)
    {
        ensureNetwork();
        for (const QString& urlString : urls) {
            const QUrl url(urlString);
            const QString key = cacheKeyForUrl(url);
            if (key.isEmpty() || m_prefetchSeen.contains(key))
                continue;
            m_prefetchSeen.insert(key);
            m_prefetchQueue.enqueue(url);
        }
        drainPrefetch();
    }

    void cancelPrefetches()
    {
        m_prefetchQueue.clear();
        m_prefetchSeen.clear();
        const auto replies = m_prefetchReplies;
        for (QNetworkReply *reply : replies) {
            if (reply)
                reply->abort();
        }
    }

    void setAuthorizationHeader(QString header)
    {
        m_authorizationHeader = header.toUtf8();
    }

    void cancelAll()
    {
        m_renderQueue.clear();
        const auto renderReplies = m_renderReplies;
        for (QNetworkReply *reply : renderReplies) {
            if (reply)
                reply->abort();
        }
        cancelPrefetches();
    }

private:
    struct RenderRequest {
        int requestId = 0;
        QUrl url;
        qint64 queuedNs = 0;
    };

    void ensureNetwork()
    {
        if (m_network)
            return;
        m_network = new QNetworkAccessManager(this);
        if (m_tlsTrust)
            m_tlsTrust->attachNetworkAccessManager(m_network, QStringLiteral("Artwork"));
        if (!m_cacheDirectory.isEmpty() && m_networkCacheBytes > 0) {
            QDir().mkpath(m_cacheDirectory);
            auto *diskCache = new QNetworkDiskCache(m_network);
            diskCache->setCacheDirectory(m_cacheDirectory);
            diskCache->setMaximumCacheSize(m_networkCacheBytes);
            m_network->setCache(diskCache);
        }
    }

    QNetworkRequest cachedRequest(const QUrl& url) const
    {
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
        if (!m_authorizationHeader.isEmpty())
            request.setRawHeader("Authorization", m_authorizationHeader);
        return request;
    }

    void drainRender()
    {
        ensureNetwork();
        while (m_network && m_renderReplies.size() < kRenderConcurrency && !m_renderQueue.isEmpty()) {
            const RenderRequest request = m_renderQueue.dequeue();
            const qint64 fetchStartedNs = monotonicNs();
            QNetworkReply *reply = m_network->get(cachedRequest(request.url));
            m_renderReplies.insert(request.requestId, reply);
            const QString key = cacheKeyForUrl(request.url);
            connect(reply, &QNetworkReply::finished, this,
                [this, reply, requestId = request.requestId, key, queuedNs = request.queuedNs, fetchStartedNs]() {
                    m_renderReplies.remove(requestId);
                    const bool diskCache = reply->attribute(QNetworkRequest::SourceIsFromCacheAttribute).toBool();
                    const qint64 finishedNs = monotonicNs();
                    finishReply(reply,
                        [this, requestId, key, diskCache, queuedNs, fetchStartedNs, finishedNs](
                            QByteArray bytes, QString error) {
                            deliverRender(requestId, key, std::move(bytes), std::move(error), diskCache,
                                std::max<qint64>(0, fetchStartedNs - queuedNs),
                                std::max<qint64>(0, finishedNs - fetchStartedNs));
                        });
                    drainRender();
                    drainPrefetch();
                });
        }
    }

    void drainPrefetch()
    {
        ensureNetwork();
        if (!m_network || !m_renderQueue.isEmpty() || !m_renderReplies.isEmpty())
            return;
        while (m_prefetchReplies.size() < m_prefetchMaxConcurrent && !m_prefetchQueue.isEmpty()) {
            const QUrl url = m_prefetchQueue.dequeue();
            const QString key = cacheKeyForUrl(url);
            QNetworkReply *reply = m_network->get(cachedRequest(url));
            m_prefetchReplies.insert(reply);
            connect(reply, &QNetworkReply::finished, this, [this, reply, key]() {
                m_prefetchReplies.remove(reply);
                m_prefetchSeen.remove(key);
                finishReply(reply, [this, key](QByteArray bytes, QString error) {
                    if (error.isEmpty() && !bytes.isEmpty())
                        deliverPrefetch(key, std::move(bytes));
                });
                drainPrefetch();
            });
        }
    }

    template <typename Callback> void finishReply(QNetworkReply *reply, Callback callback)
    {
        if (!reply) {
            callback({}, QStringLiteral("Network reply disappeared"));
            return;
        }

        const bool ok = reply->error() == QNetworkReply::NoError;
        QByteArray bytes = ok && reply->isReadable() ? reply->readAll() : QByteArray();
        const QString error = ok ? QString() : reply->errorString();
        reply->deleteLater();
        if (ok && bytes.isEmpty())
            callback({}, QStringLiteral("Artwork response was empty"));
        else
            callback(std::move(bytes), error);
    }

    void deliverRender(int requestId, QString key, QByteArray bytes, QString error, bool diskCache, qint64 queueNs = 0,
        qint64 fetchNs = 0)
    {
        QPointer<ArtworkService> service = m_service;
        QMetaObject::invokeMethod(
            m_service,
            [service, requestId, key = std::move(key), bytes = std::move(bytes), error = std::move(error), diskCache,
                queueNs, fetchNs]() mutable {
                if (service)
                    service->handleRenderFetched(
                        requestId, std::move(key), std::move(bytes), std::move(error), diskCache, queueNs, fetchNs);
            },
            Qt::QueuedConnection);
    }

    void deliverPrefetch(QString key, QByteArray bytes)
    {
        QPointer<ArtworkService> service = m_service;
        QMetaObject::invokeMethod(
            m_service,
            [service, key = std::move(key), bytes = std::move(bytes)]() mutable {
                if (service)
                    service->handlePrefetched(std::move(key), std::move(bytes));
            },
            Qt::QueuedConnection);
    }

    QString m_cacheDirectory;
    qint64 m_networkCacheBytes = 0;
    QPointer<ArtworkService> m_service;
    TlsTrustController *m_tlsTrust = nullptr;
    QByteArray m_authorizationHeader;
    QNetworkAccessManager *m_network = nullptr;
    QQueue<RenderRequest> m_renderQueue;
    QHash<int, QNetworkReply *> m_renderReplies;
    QQueue<QUrl> m_prefetchQueue;
    QSet<QString> m_prefetchSeen;
    QSet<QNetworkReply *> m_prefetchReplies;
    int m_prefetchMaxConcurrent = 3;
};

class ArtworkImageResponse final : public QQuickImageResponse {
public:
    ArtworkImageResponse(ArtworkService *service, QUrl url, QSize requestedSize)
        : m_service(service)
        , m_requestedSize(std::move(requestedSize))
    {
        if (!m_service) {
            finish({}, QStringLiteral("Artwork service unavailable"));
            return;
        }
        m_requestId = m_service->requestImage(std::move(url), m_requestedSize, this);
    }

    ~ArtworkImageResponse() override = default;

    QQuickTextureFactory *textureFactory() const override
    {
        return m_image.isNull() ? nullptr : QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    QString errorString() const override
    {
        return m_error;
    }

    void cancel() override
    {
        m_cancelled.store(true);
        if (m_service && m_requestId > 0)
            m_service->cancelRequest(m_requestId);
    }

    bool cancelled() const
    {
        return m_cancelled.load();
    }

    QSize requestedSize() const
    {
        return m_requestedSize;
    }

    void finish(QImage image, QString error)
    {
        if (m_finished.exchange(true))
            return;
        if (m_cancelled.load() && error.isEmpty()) {
            m_error = QStringLiteral("Cancelled");
        } else if (image.isNull() && !error.isEmpty()) {
            m_image = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
            m_image.fill(Qt::transparent);
        } else {
            m_image = std::move(image);
            m_error = std::move(error);
        }
        emit finished();
    }

private:
    QPointer<ArtworkService> m_service;
    QSize m_requestedSize;
    int m_requestId = 0;
    QImage m_image;
    QString m_error;
    std::atomic_bool m_cancelled = false;
    std::atomic_bool m_finished = false;
};

ArtworkByteCache::ArtworkByteCache(int maximumBytes)
{
    m_cache.setMaxCost(std::max(1, maximumBytes));
}

QByteArray ArtworkByteCache::get(const QString& key)
{
    QMutexLocker locker(&m_mutex);
    const QByteArray *bytes = m_cache.object(key);
    return bytes ? *bytes : QByteArray();
}

void ArtworkByteCache::insert(const QString& key, QByteArray bytes)
{
    if (bytes.isEmpty())
        return;
    auto *stored = new QByteArray(std::move(bytes));
    const int cost = stored->size();
    QMutexLocker locker(&m_mutex);
    m_cache.insert(key, stored, cost);
}

void ArtworkByteCache::clear()
{
    QMutexLocker locker(&m_mutex);
    m_cache.clear();
}

ArtworkService::ArtworkService(QString cacheDirectory, qint64 networkCacheBytes, int byteCacheBytes, int decodeThreads,
    TlsTrustController *tlsTrust, QObject *parent)
    : QObject(parent)
    , m_cacheDirectory(std::move(cacheDirectory))
    , m_networkCacheBytes(networkCacheBytes)
    , m_byteCache(std::make_shared<ArtworkByteCache>(byteCacheBytes))
{
    m_decodePool.setMaxThreadCount(std::max(1, decodeThreads));
    m_decodePool.setExpiryTimeout(30000);
    m_timingBatchTimer.setSingleShot(true);
    m_timingBatchTimer.setInterval(120);
    connect(&m_timingBatchTimer, &QTimer::timeout, this, &ArtworkService::flushTimingBatch);

    m_deliveryTimer.setInterval(16);
    m_deliveryTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_deliveryTimer, &QTimer::timeout, this, [this] {
        m_deliveredThisTick = 0;
        drainDeliveries();
        if (m_pendingDeliveries.empty())
            m_deliveryTimer.stop();
    });

    m_worker = new ArtworkFetchWorker(m_cacheDirectory, m_networkCacheBytes, this, tlsTrust);
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_workerThread.start();
}

ArtworkService::~ArtworkService()
{
    invokeWorker([](ArtworkFetchWorker *worker) { worker->cancelAll(); });
    m_workerThread.quit();
    m_workerThread.wait();
    m_decodePool.waitForDone();
}

QString ArtworkService::url(const QVariant& value, const QString& kind, int width) const
{
    if (value.canConvert<MovieItem>())
        return movieUrl(value.value<MovieItem>(), kind, width);
    if (value.metaType().id() == QMetaType::QVariantMap) {
        const QVariantMap map = value.toMap();
        MovieItem item;
        item.id = map.value(QStringLiteral("movieId")).toString();
        item.posterTag = map.value(QStringLiteral("posterTag")).toString();
        item.albumId = map.value(QStringLiteral("albumId")).toString();
        item.albumPrimaryImageTag = map.value(QStringLiteral("albumPrimaryImageTag")).toString();
        return movieUrl(item, kind, width);
    }
    if (value.canConvert<LibraryItem>()) {
        const LibraryItem item = value.value<LibraryItem>();
        return buildUrl(item.id, item.imageTag, QStringLiteral("Primary"), width > 0 ? width : 280, 0);
    }
    if (value.canConvert<PersonItem>()) {
        const PersonItem item = value.value<PersonItem>();
        return buildUrl(item.id, item.imageTag, QStringLiteral("Primary"), width > 0 ? width : 360, 5);
    }
    return {};
}

QString ArtworkService::itemUrl(const MovieItem& item, bool landscape, int width) const
{
    return movieUrl(item, landscape ? QStringLiteral("landscape") : QStringLiteral("poster"), width);
}

void ArtworkService::setSource(const ArtworkSource *source)
{
    m_source = source;
}

void ArtworkService::setUiWidth(int width)
{
    if (width > 0)
        m_uiWidth = width;
}

QString ArtworkService::artworkFormat() const
{
    return m_artworkFormat.isEmpty() ? QString::fromLatin1(platformDefaultArtworkFormat()) : m_artworkFormat;
}

int ArtworkService::qualityForOffset(int offset) const
{
    const int base = artworkFormat() == QLatin1String("jpeg") ? m_jpegQuality : m_webpQuality;
    return std::clamp(base + offset, 1, 100);
}

void ArtworkService::setArtworkEncoding(const QString& format, int webpQuality, int jpegQuality)
{
    const QString normalized = (format == QLatin1String("webp") || format == QLatin1String("jpeg"))
        ? format
        : QString::fromLatin1(platformDefaultArtworkFormat());
    m_artworkFormat = normalized;
    m_webpQuality = std::clamp(webpQuality, 1, 100);
    m_jpegQuality = std::clamp(jpegQuality, 1, 100);
}

QString ArtworkService::movieUrl(const MovieItem& item, const QString& kind, int width) const
{
    QString itemId = item.id;
    QString tag = item.posterTag;
    QString imageType = QStringLiteral("Primary");
    int maxWidth = width > 0 ? width : 280;
    int qualityOffset = 0;
    int fillWidth = 0;
    int fillHeight = 0;

    if (tag.isEmpty() && !item.albumPrimaryImageTag.isEmpty() && !item.albumId.isEmpty()
        && kind != QStringLiteral("logo") && kind != QStringLiteral("banner")) {
        // A track rarely carries its own cover; the album holds it.
        itemId = item.albumId;
        tag = item.albumPrimaryImageTag;
    }

    if (kind == QStringLiteral("seriesPoster")) {
        if (!item.seriesId.isEmpty() && !item.seriesPrimaryImageTag.isEmpty()) {
            itemId = item.seriesId;
            tag = item.seriesPrimaryImageTag;
        }
        maxWidth = width > 0 ? width : 360;
        qualityOffset = 5;
    } else if (kind == QStringLiteral("square")) {
        maxWidth = width > 0 ? width : 320;
        qualityOffset = 5;
    } else if (kind == QStringLiteral("landscape")) {
        if (!item.thumbTag.isEmpty()) {
            tag = item.thumbTag;
            imageType = QStringLiteral("Thumb");
        } else if (!item.backdropTag.isEmpty()) {
            tag = item.backdropTag;
            imageType = QStringLiteral("Backdrop");
        } else if (tag.isEmpty() && !item.seriesId.isEmpty()) {
            itemId = item.seriesId;
            tag = item.seriesPrimaryImageTag;
        }
        maxWidth = width > 0 ? width : (m_uiWidth >= 3000 ? 640 : 400);
        qualityOffset = m_uiWidth >= 3000 ? -5 : -7;
        if (imageType != QStringLiteral("Primary")) {
            fillWidth = maxWidth;
            fillHeight = maxWidth * 9 / 16;
        }
    } else if (kind == QStringLiteral("backdrop")) {
        if (!item.backdropTag.isEmpty()) {
            tag = item.backdropTag;
            imageType = QStringLiteral("Backdrop");
        } else if (!item.thumbTag.isEmpty()) {
            tag = item.thumbTag;
            imageType = QStringLiteral("Thumb");
        }
        maxWidth = width > 0 ? width : 1920;
        qualityOffset = 7;
    } else if (kind == QStringLiteral("logo")) {
        // Logos need their transparency, so they are PNG whatever the user
        // picked. PNG is lossless, which makes the quality parameter inert --
        // pinning it keeps every client on one URL for the same logo.
        tag = item.logoTag;
        imageType = QStringLiteral("Logo");
        maxWidth = width > 0 ? width : 720;
        return buildLosslessUrl(itemId, tag, imageType, maxWidth);
    } else if (kind == QStringLiteral("banner")) {
        tag = item.bannerTag;
        imageType = QStringLiteral("Banner");
        maxWidth = width > 0 ? width : 1000;
        qualityOffset = 11;
    } else if (kind == QStringLiteral("thumb")) {
        tag = item.thumbTag;
        imageType = QStringLiteral("Thumb");
        maxWidth = width > 0 ? width : 720;
        qualityOffset = 7;
    }
    return buildUrl(itemId, tag, imageType, maxWidth, qualityOffset, fillWidth, fillHeight);
}

QString ArtworkService::buildLosslessUrl(
    const QString& itemId, const QString& tag, const QString& imageType, int maxWidth) const
{
    if (!m_source)
        return {};
    ArtworkSource::ImageRequest request;
    request.itemId = itemId;
    request.tag = tag;
    request.imageType = imageType;
    request.maxWidth = maxWidth;
    request.format = QStringLiteral("png");
    request.quality = 90;
    return m_source->imageUrl(request);
}

QString ArtworkService::buildUrl(const QString& itemId, const QString& tag, const QString& imageType, int maxWidth,
    int qualityOffset, int fillWidth, int fillHeight) const
{
    if (!m_source)
        return {};
    ArtworkSource::ImageRequest request;
    request.itemId = itemId;
    request.tag = tag;
    request.imageType = imageType;
    request.maxWidth = maxWidth;
    request.fillWidth = fillWidth;
    request.fillHeight = fillHeight;
    request.format = artworkFormat();
    request.quality = qualityForOffset(qualityOffset);
    return m_source->imageUrl(request);
}

QQuickImageResponse *ArtworkService::requestImageResponse(const QString& id, const QSize& requestedSize)
{
    const QString urlString = QUrl::fromPercentEncoding(id.toUtf8());
    return new ArtworkImageResponse(this, QUrl(urlString), requestedSize);
}

void ArtworkService::prefetch(const QStringList& urls)
{
    QStringList uncached;
    uncached.reserve(urls.size());
    for (const QString& url : urls) {
        const QString key = cacheKeyForUrl(QUrl(url));
        if (key.isEmpty() || !m_byteCache || !m_byteCache->get(key).isEmpty())
            continue;
        uncached.push_back(url);
    }
    if (uncached.isEmpty())
        return;
    invokeWorker(
        [urls = std::move(uncached)](ArtworkFetchWorker *worker) mutable { worker->prefetch(std::move(urls)); });
}

ArtworkService::DecodeTotals ArtworkService::decodeTotals() const
{
    QMutexLocker locker(&m_decodeTotalsMutex);
    return m_decodeTotals;
}

int ArtworkService::outstandingRequests() const
{
    return static_cast<int>(m_responses.size()) + static_cast<int>(m_pendingDeliveries.size());
}

void ArtworkService::setAuthorizationHeader(QString header)
{
    invokeWorker([header = std::move(header)](
                     ArtworkFetchWorker *worker) mutable { worker->setAuthorizationHeader(std::move(header)); });
}

void ArtworkService::cancelPrefetches()
{
    invokeWorker([](ArtworkFetchWorker *worker) { worker->cancelPrefetches(); });
}

void ArtworkService::releaseMemory(bool aggressive)
{
    cancelPrefetches();
    if (m_byteCache)
        m_byteCache->clear();
    if (aggressive)
        m_decodePool.clear();
}

int ArtworkService::requestImage(QUrl url, QSize requestedSize, ArtworkImageResponse *response)
{
    if (!url.isValid() || url.scheme().isEmpty()) {
        if (response)
            response->finish({}, QStringLiteral("Invalid artwork URL"));
        return 0;
    }

    const QString key = cacheKeyForUrl(url);
    const QByteArray cached = m_byteCache ? m_byteCache->get(key) : QByteArray();
    if (!cached.isEmpty()) {
        Timing timing;
        timing.memoryCache = true;
        timing.requestedSize = requestedSize;
        timing.totalNs = monotonicNs();
        startDecode(response, cached, timing);
        return 0;
    }

    const int requestId = m_nextRequestId++;
    m_responses.insert(requestId, response);
    m_requestStarts.insert(requestId, monotonicNs());
    invokeWorker([requestId, url = std::move(url)](
                     ArtworkFetchWorker *worker) mutable { worker->fetchRender(requestId, std::move(url)); });
    return requestId;
}

void ArtworkService::cancelRequest(int requestId)
{
    if (requestId <= 0)
        return;
    m_responses.remove(requestId);
    Timing timing;
    timing.cancelled = true;
    const qint64 startedNs = m_requestStarts.take(requestId);
    timing.totalNs = startedNs > 0 ? monotonicNs() - startedNs : 0;
    finishTiming(timing);
    invokeWorker([requestId](ArtworkFetchWorker *worker) { worker->cancelRender(requestId); });
}

void ArtworkService::handleRenderFetched(
    int requestId, QString key, QByteArray bytes, QString error, bool diskCache, qint64 queueNs, qint64 fetchNs)
{
    const qint64 startedNs = m_requestStarts.take(requestId);
    const QPointer<ArtworkImageResponse> response = m_responses.take(requestId);
    if (!response)
        return;
    Timing timing;
    timing.diskCache = diskCache;
    timing.network = !diskCache;
    timing.queueNs = queueNs;
    timing.fetchNs = fetchNs;
    timing.requestedSize = response->requestedSize();
    timing.totalNs = startedNs > 0 ? startedNs : monotonicNs();
    if (!error.isEmpty() || bytes.isEmpty()) {
        response->finish({}, std::move(error));
        timing.totalNs = monotonicNs() - timing.totalNs;
        finishTiming(timing);
        return;
    }
    if (m_byteCache)
        m_byteCache->insert(key, bytes);
    startDecode(response, std::move(bytes), timing);
}

void ArtworkService::handlePrefetched(QString key, QByteArray bytes)
{
    if (m_byteCache)
        m_byteCache->insert(key, std::move(bytes));
}

void ArtworkService::startDecode(ArtworkImageResponse *response, QByteArray bytes, Timing timing)
{
    if (!response) {
        return;
    }
    const QPointer<ArtworkImageResponse> self(response);
    const qint64 queuedNs = monotonicNs();
    m_decodePool.start(QRunnable::create([this, self, bytes = std::move(bytes), timing, queuedNs]() mutable {
        if (!self || self->cancelled())
            return;
        const qint64 decodeStartedNs = monotonicNs();
        QString error;
        QImage image = decodeArtwork(bytes, timing.requestedSize, &error);
        const qint64 decodeFinishedNs = monotonicNs();
        if (!self || self->cancelled())
            return;
        timing.decodeQueueNs = std::max<qint64>(0, decodeStartedNs - queuedNs);
        timing.decodeNs = std::max<qint64>(0, decodeFinishedNs - decodeStartedNs);
        {
            QMutexLocker locker(&m_decodeTotalsMutex);
            m_decodeTotals.decodeNs += timing.decodeNs;
            m_decodeTotals.pixels += static_cast<qint64>(image.width()) * image.height();
            m_decodeTotals.images += 1;
        }
        timing.resultSize = image.size();
        QMetaObject::invokeMethod(
            this,
            [this, self, image = std::move(image), error = std::move(error), timing]() mutable {
                enqueueDelivery([this, self, image = std::move(image), error = std::move(error), timing]() mutable {
                    if (!self)
                        return;
                    self->finish(std::move(image), std::move(error));
                    Timing completed = timing;
                    completed.totalNs = monotonicNs() - completed.totalNs;
                    finishTiming(completed);
                });
            },
            Qt::QueuedConnection);
    }));
}

void ArtworkService::enqueueDelivery(std::function<void()> deliver)
{
    m_pendingDeliveries.push_back(std::move(deliver));
    if (!m_deliveryTimer.isActive()) {
        m_deliveredThisTick = 0;
        m_deliveryTimer.start();
    }
    drainDeliveries();
}

void ArtworkService::drainDeliveries()
{
    while (!m_pendingDeliveries.empty() && m_deliveredThisTick < kMaxDeliveriesPerTick) {
        auto deliver = std::move(m_pendingDeliveries.front());
        m_pendingDeliveries.pop_front();
        ++m_deliveredThisTick;
        deliver();
    }
}

void ArtworkService::finishTiming(Timing timing)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this, [this, timing = std::move(timing)]() mutable { finishTiming(std::move(timing)); },
            Qt::QueuedConnection);
        return;
    }

    m_timingBatch.push_back(std::move(timing));
    m_timingBatchTimer.start();
}

void ArtworkService::flushTimingBatch()
{
    if (m_timingBatch.isEmpty())
        return;
    auto percentile = [](QList<qint64> values, double fraction) {
        if (values.isEmpty())
            return qint64(0);
        std::sort(values.begin(), values.end());
        return values.at(std::min(values.size() - 1, static_cast<qsizetype>(std::floor(values.size() * fraction))));
    };
    QList<qint64> queues;
    QList<qint64> fetches;
    QList<qint64> decodes;
    qint64 deliveredLastNs = 0;
    int memory = 0;
    int disk = 0;
    int network = 0;
    int cancelled = 0;
    QSize requestedSize;
    QSize resultSize;
    for (const Timing& timing : std::as_const(m_timingBatch)) {
        memory += timing.memoryCache;
        disk += timing.diskCache;
        network += timing.network;
        cancelled += timing.cancelled;
        queues.push_back(timing.queueNs + timing.decodeQueueNs);
        if (timing.fetchNs > 0)
            fetches.push_back(timing.fetchNs);
        if (timing.decodeNs > 0)
            decodes.push_back(timing.decodeNs);
        deliveredLastNs = std::max(deliveredLastNs, timing.totalNs);
        if (!requestedSize.isValid() && timing.requestedSize.isValid())
            requestedSize = timing.requestedSize;
        if (!resultSize.isValid() && timing.resultSize.isValid())
            resultSize = timing.resultSize;
    }
    const auto ms = [](qint64 ns) { return QString::number(static_cast<double>(ns) / 1000000.0, 'f', 2); };
    qInfo().noquote() << QStringLiteral(
        "artwork batch: n=%1 mem=%2 disk=%3 net=%4 queue_p50=%5 fetch_p50=%6 fetch_p95=%7 "
        "decode_p50=%8 decode_p95=%9 deliver_last_ms=%10 requested=%11x%12 result=%13x%14 cancelled=%15")
                             .arg(m_timingBatch.size())
                             .arg(memory)
                             .arg(disk)
                             .arg(network)
                             .arg(ms(percentile(queues, 0.50)), ms(percentile(fetches, 0.50)),
                                 ms(percentile(fetches, 0.95)), ms(percentile(decodes, 0.50)),
                                 ms(percentile(decodes, 0.95)), ms(deliveredLastNs))
                             .arg(requestedSize.width())
                             .arg(requestedSize.height())
                             .arg(resultSize.width())
                             .arg(resultSize.height())
                             .arg(cancelled);
    m_timingBatch.clear();
}

void ArtworkService::invokeWorker(std::function<void(ArtworkFetchWorker *)> call)
{
    if (!m_worker)
        return;
    QPointer<ArtworkFetchWorker> worker = m_worker;
    QMetaObject::invokeMethod(
        m_worker,
        [worker, call = std::move(call)]() mutable {
            if (worker)
                call(worker);
        },
        Qt::QueuedConnection);
}

ArtworkImageProvider::ArtworkImageProvider(ArtworkService *service)
    : m_service(service)
{
}

QQuickImageResponse *ArtworkImageProvider::requestImageResponse(const QString& id, const QSize& requestedSize)
{
    return m_service ? m_service->requestImageResponse(id, requestedSize)
                     : new ArtworkImageResponse(nullptr, {}, requestedSize);
}

} // namespace JellyfinNative
