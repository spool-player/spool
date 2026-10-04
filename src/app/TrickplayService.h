#pragma once

#include "../media/MediaTypes.h"
#include "../provider/ArtworkSource.h"
#include "TrickplayDecoder.h"
#include <QCache>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQuickAsyncImageProvider>
#include <QThreadPool>
#include <QVariantMap>
#include <atomic>
#include <memory>
#include <utility>

class QNetworkAccessManager;
class QNetworkReply;

namespace Spool {
class TlsTrustController;
class TrickplayImageResponse;

class TrickplayService final : public QObject {
    Q_OBJECT
public:
    TrickplayService(const ArtworkSource *source, TlsTrustController *trust, QObject *parent = nullptr);
    ~TrickplayService() override;
    void setSession(const TrickplayInfo& info, double startSeconds);
    void setImageProviderName(QString name)
    {
        m_providerName = std::move(name);
    }
    void clear();
    bool available() const;
    QVariantMap frame(double seconds);
    QQuickImageResponse *requestImageResponse(const QString& id);
signals:
    void changed();

private:
    friend class TrickplayImageResponse;
    bool residentSheet() const;
    int textureIndex(int index) const;
    QString resourceUrl(int index) const;
    bool authorize(int index);
    void select(int index);
    void supersede(int preserveIndex);
    void request(int index, TrickplayImageResponse *response);
    void fetch(int index, bool speculative = false);
    void abortFetch();
    void decode(int index, QByteArray backing, bool speculative = false, qsizetype offset = 0, qsizetype length = -1);
    void startDecode(int index, bool speculative = false);
    void prefetch();
    void complete(int index, QImage image, const QString& error, bool speculative);
    const ArtworkSource *m_source;
    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_reply;
    QThreadPool m_pool;
    TrickplayInfo m_info;
    BifSequence m_bif;
    QCache<int, QByteArray> m_sheets { 32 * 1024 * 1024 };
    QCache<int, QImage> m_images { int(TrickplayDecodedByteBudget) };
    QHash<int, QList<QPointer<TrickplayImageResponse>>> m_waiters;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    std::shared_ptr<std::atomic_bool> m_decodeCancelled;
    QString m_fetchUrl;
    QByteArray m_fetchBytes;
    QString m_providerName = QStringLiteral("trickplay");
    quint64 m_generation = 0;
    bool m_decoding = false;
    bool m_decodeSpeculative = false;
    bool m_fetchSpeculative = false;
    int m_decodeIndex = -1;
    int m_fetchIndex = -1;
    int m_pendingIndex = -1;
    int m_selectedIndex = -1;
    int m_direction = 1;
    int m_prefetchAnchor = -1;
    int m_prefetchDirection = 0;
    int m_prefetchAttempts = 0;
    double m_startSeconds = 0;
};

class TrickplayImageProvider final : public QQuickAsyncImageProvider {
public:
    explicit TrickplayImageProvider(TrickplayService *service)
        : m_service(service)
    {
    }
    QQuickImageResponse *requestImageResponse(const QString& id, const QSize&) override;

private:
    QPointer<TrickplayService> m_service;
};
} // namespace Spool
