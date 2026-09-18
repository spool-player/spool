#pragma once

#include "../media/MediaTypes.h"
#include "../provider/ArtworkSource.h"
#include "ArtworkPrefetcher.h"

#include <QByteArray>
#include <QCache>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <deque>
#include <functional>
#include <memory>

class QQuickImageResponse;

namespace JellyfinNative {

class ArtworkByteCache final {
public:
    explicit ArtworkByteCache(int maximumBytes);

    QByteArray get(const QString& key);
    void insert(const QString& key, QByteArray bytes);
    void clear();

private:
    QMutex m_mutex;
    QCache<QString, QByteArray> m_cache;
};

class TlsTrustController;
class ArtworkFetchWorker;
class ArtworkImageResponse;

class ArtworkService final : public QObject, public ArtworkPrefetcher {
    Q_OBJECT
public:
    ArtworkService(QString cacheDirectory, qint64 networkCacheBytes, int byteCacheBytes, int decodeThreads,
        TlsTrustController *tlsTrust, QObject *parent = nullptr);
    ~ArtworkService() override;

    Q_INVOKABLE QString url(const QVariant& item, const QString& kind, int width = 0) const;
    QString itemUrl(const MovieItem& item, bool landscape, int width = 0) const override;
    // Where image URLs come from. Unset, every request resolves to no URL.
    void setSource(const ArtworkSource *source);
    void setUiWidth(int width);
    // Which codec to ask the server for, and how hard it should compress.
    // Format is "webp" or "jpeg"; anything else falls back to the platform
    // default. The two qualities are kept apart because the same number means
    // different things to the two encoders -- JPEG needs about 82 to match
    // what WebP does at 75 -- and because a client that switches format
    // should land on URLs other clients of that format already asked for, so
    // the server's own encoded-image cache is shared rather than duplicated.
    void setArtworkEncoding(const QString& format, int webpQuality, int jpegQuality);
    QQuickImageResponse *requestImageResponse(const QString& id, const QSize& requestedSize);
    void prefetch(const QStringList& urls) override;
    void cancelPrefetches() override;
    void releaseMemory(bool aggressive);
    void setAuthorizationHeader(QString header);
    // How much artwork is still on its way: requests that have not produced an
    // image yet, plus images decoded but not yet handed to the scene graph.
    // Zero means everything asked for has arrived, which is what "wait until
    // the pictures are there" means for an automated run.
    int outstandingRequests() const;
    // Cumulative decode work since the process started: how long decoding has
    // taken on the pool threads, and how many pixels came out of it. Both are
    // what a source size actually changes, and unlike wall-clock settle time
    // they do not depend on how fast the GPU or the network happen to be.
    struct DecodeTotals {
        qint64 decodeNs = 0;
        qint64 pixels = 0;
        int images = 0;
    };
    DecodeTotals decodeTotals() const;

    int requestImage(QUrl url, QSize requestedSize, ArtworkImageResponse *response);
    void cancelRequest(int requestId);
    void handleRenderFetched(
        int requestId, QString key, QByteArray bytes, QString error, bool diskCache, qint64 queueNs, qint64 fetchNs);
    void handlePrefetched(QString key, QByteArray bytes);

private:
    struct Timing {
        bool memoryCache = false;
        bool diskCache = false;
        bool network = false;
        bool cancelled = false;
        qint64 queueNs = 0;
        qint64 fetchNs = 0;
        qint64 decodeQueueNs = 0;
        qint64 decodeNs = 0;
        qint64 totalNs = 0;
        QSize requestedSize;
        QSize resultSize;
    };
    void startDecode(ArtworkImageResponse *response, QByteArray bytes, Timing timing);
    void enqueueDelivery(std::function<void()> deliver);
    void drainDeliveries();
    void finishTiming(Timing timing);
    void flushTimingBatch();
    void invokeWorker(std::function<void(ArtworkFetchWorker *)> call);
    QString movieUrl(const MovieItem& item, const QString& kind, int width) const;
    QString buildUrl(const QString& itemId, const QString& tag, const QString& imageType, int maxWidth,
        int qualityOffset, int fillWidth = 0, int fillHeight = 0) const;
    QString buildLosslessUrl(const QString& itemId, const QString& tag, const QString& imageType, int maxWidth) const;
    // Lossy artwork is requested at the user's base quality shifted by a
    // per-kind offset, so one slider moves every kind together while keeping
    // the relative tuning between them. The offsets are relative to a poster,
    // and the shipped defaults reproduce the qualities the app requested
    // before the setting existed.
    int qualityForOffset(int offset) const;
    QString artworkFormat() const;

    QString m_cacheDirectory;
    const ArtworkSource *m_source = nullptr;
    QString m_artworkFormat;
    int m_webpQuality = 75;
    int m_jpegQuality = 82;
    int m_uiWidth = 1920;
    qint64 m_networkCacheBytes = 0;
    std::shared_ptr<ArtworkByteCache> m_byteCache;
    mutable QMutex m_decodeTotalsMutex;
    DecodeTotals m_decodeTotals;
    QThreadPool m_decodePool;
    QThread m_workerThread;
    ArtworkFetchWorker *m_worker = nullptr;
    QHash<int, QPointer<ArtworkImageResponse>> m_responses;
    QHash<int, qint64> m_requestStarts;
    QList<Timing> m_timingBatch;
    QTimer m_timingBatchTimer;
    // Decoded images ready for the scene graph; completed a few per frame so
    // texture uploads from large batches don't clump into one long frame.
    std::deque<std::function<void()>> m_pendingDeliveries;
    QTimer m_deliveryTimer;
    int m_deliveredThisTick = 0;
    int m_nextRequestId = 1;
};

} // namespace JellyfinNative
