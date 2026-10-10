#include "TrickplayService.h"
#include "../common/TlsTrust.h"
#include "../provider/ProviderLogging.h"

#include <QCoreApplication>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQuickTextureFactory>
#include <QQuickWindow>
#include <QRunnable>
#include <cmath>
#include <limits>
#include <utility>

namespace Spool {
namespace {
    constexpr qsizetype MaximumBytes = 32 * 1024 * 1024;
}

class TrickplayImageResponse final : public QQuickImageResponse {
public:
    TrickplayImageResponse(TrickplayService *service, QString id)
        : m_service(service)
    {
        QObject *context = service ? static_cast<QObject *>(service) : QCoreApplication::instance();
        if (context && thread() != context->thread())
            moveToThread(context->thread());
        const QPointer<TrickplayImageResponse> self(this);
        QMetaObject::invokeMethod(
            context,
            [self, id = std::move(id)] {
                if (!self)
                    return;
                const auto parts = id.split('/');
                bool generationOk = false, indexOk = false;
                const quint64 generation = parts.value(0).toULongLong(&generationOk);
                const int index = parts.value(1).toInt(&indexOk);
                if (!self->m_service || !generationOk || !indexOk || index < 0
                    || generation != self->m_service->m_generation || self->m_cancelled)
                    self->finish({}, QStringLiteral("Stale preview request"));
                else
                    self->m_service->request(index, self);
            },
            Qt::QueuedConnection);
    }
    QQuickTextureFactory *textureFactory() const override
    {
        return m_image ? new TrickplayTextureFactory(m_image) : nullptr;
    }
    QString errorString() const override
    {
        return m_error;
    }
    void cancel() override
    {
        m_cancelled = true;
        const QPointer<TrickplayImageResponse> self(this);
        QObject *context = m_service ? static_cast<QObject *>(m_service.data()) : QCoreApplication::instance();
        QMetaObject::invokeMethod(
            context,
            [self] {
                if (self)
                    self->finish({}, QStringLiteral("Cancelled"));
            },
            Qt::QueuedConnection);
    }
    void finish(std::shared_ptr<const TrickplayTexture> image, QString error)
    {
        if (m_finished.exchange(true))
            return;
        m_image = m_cancelled ? nullptr : std::move(image);
        m_error = m_cancelled ? QStringLiteral("Cancelled") : std::move(error);
        emit finished();
    }

private:
    QPointer<TrickplayService> m_service;
    std::shared_ptr<const TrickplayTexture> m_image;
    QString m_error;
    std::atomic_bool m_cancelled = false;
    std::atomic_bool m_finished = false;
};

TrickplayTextureFactory::TrickplayTextureFactory(std::shared_ptr<const TrickplayTexture> frame)
    : texture(std::move(frame))
{
}

QSGTexture *TrickplayTextureFactory::createTexture(QQuickWindow *window) const
{
    return window->createTextureFromImage(image());
}

QSize TrickplayTextureFactory::textureSize() const
{
    return texture->size;
}
int TrickplayTextureFactory::textureByteCount() const
{
    return int(qint64(texture->size.width()) * texture->size.height() * 4);
}
QImage TrickplayTextureFactory::image() const
{
    return texture->image();
}

TrickplayService::TrickplayService(const ArtworkSource *source, TlsTrustController *trust, QObject *parent)
    : QObject(parent)
    , m_source(source)
    , m_network(new QNetworkAccessManager(this))
{
    // Independent connections, decode pool, and delivery: never queue behind
    // posters or their rate-limited scene graph deliveries.
    m_pool.setMaxThreadCount(1);
    if (trust)
        trust->attachNetworkAccessManager(m_network, QStringLiteral("Playback preview"));
    m_cancelled = std::make_shared<std::atomic_bool>(false);
}

TrickplayService::~TrickplayService()
{
    clear();
    m_pool.waitForDone();
}

void TrickplayService::clear()
{
    ++m_generation;
    m_cancelled->store(true);
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    if (m_decodeCancelled)
        m_decodeCancelled->store(true);
    abortFetch();
    m_pool.clear();
    m_decoding = false;
    m_decodeIndex = m_pendingIndex = m_selectedIndex = -1;
    m_prefetchAnchor = -1;
    m_prefetchDirection = m_prefetchAttempts = 0;
    m_direction = 1;
    supersede(-1);
    m_sheets.clear();
    m_images.clear();
    m_bif = {};
    m_info = {};
    emit changed();
}

void TrickplayService::setAccurateDecoding(bool accurate)
{
    if (m_accurateDecoding == accurate)
        return;
    m_accurateDecoding = accurate;
    const TrickplayInfo info = m_info;
    setSession(info, m_startSeconds);
}

void TrickplayService::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    const TrickplayInfo info = m_info;
    setSession(info, m_startSeconds);
}

void TrickplayService::setSession(const TrickplayInfo& info, double startSeconds)
{
    clear();
    m_info = info;
    m_startSeconds = startSeconds;
    if (providerLogEnabled(ProviderLogLevel::Trace)) {
        const bool bif = info.format == QLatin1String("bif");
        const bool supported = bif || info.format.isEmpty() || info.format == QLatin1String("sprites");
        const bool missing = info.url.isEmpty() && info.urlTemplate.isEmpty();
        writeProviderLog(ProviderLogLevel::Trace,
            QStringLiteral("preview session enabled=%1 availability=%2 format=%3 width=%4 height=%5 count=%6")
                .arg(m_enabled)
                .arg(missing        ? QStringLiteral("missing")
                        : supported ? QStringLiteral("supported")
                                    : QStringLiteral("unsupported"))
                .arg(bif            ? QStringLiteral("bif")
                        : supported ? QStringLiteral("sprites")
                                    : QStringLiteral("unknown"))
                .arg(info.width)
                .arg(info.height)
                .arg(info.thumbnailCount));
    }
    if (!m_enabled)
        return;
    if (info.format == QLatin1String("bif") && !info.url.isEmpty())
        fetch(0);
    else if (available()) {
        const QVariantMap first = frame(startSeconds);
        if (first.value("available").toBool())
            request(textureIndex(first.value("frameIndex").toInt()), nullptr);
    }
    emit changed();
}

bool TrickplayService::available() const
{
    if (!m_enabled)
        return false;
    if (m_info.format == QLatin1String("bif"))
        return !m_info.url.isEmpty() && m_bif.count() > 0 && m_info.width > 0 && m_info.height > 0;
    if (!m_info.format.isEmpty() && m_info.format != QLatin1String("sprites"))
        return false;
    return !m_info.urlTemplate.isEmpty() && m_info.intervalMs > 0 && m_info.width > 0 && m_info.height > 0
        && m_info.tileWidth > 0 && m_info.tileHeight > 0 && qint64(m_info.width) * m_info.height <= 1024 * 1024
        && qint64(m_info.width) * m_info.tileWidth <= 16384 && qint64(m_info.height) * m_info.tileHeight <= 16384
        && qint64(m_info.tileWidth) * m_info.tileHeight <= std::numeric_limits<int>::max();
}

bool TrickplayService::residentSheet() const
{
    return m_info.format != QLatin1String("bif")
        && qint64(m_info.width) * m_info.height * m_info.tileWidth * m_info.tileHeight * 4
        <= TrickplayDecodedByteBudget;
}

int TrickplayService::textureIndex(int index) const
{
    if (!residentSheet())
        return index;
    const int tiles = m_info.tileWidth * m_info.tileHeight;
    return index / tiles * tiles;
}

QString TrickplayService::resourceUrl(int index) const
{
    if (m_info.format == QLatin1String("bif"))
        return m_info.url;
    QString url = m_info.urlTemplate;
    url.replace(QLatin1String("{index}"), QString::number(index / (m_info.tileWidth * m_info.tileHeight)));
    return url;
}

bool TrickplayService::authorize(int index)
{
    const auto resource = m_source ? m_source->resolveImage(QUrl(resourceUrl(index))) : ArtworkSource::ImageResource {};
    if (resource.url.isValid() && !resource.url.scheme().isEmpty())
        return true;
    // Revocation also cancels speculative work and retires decoded credentials.
    clear();
    return false;
}

QVariantMap TrickplayService::frame(double seconds)
{
    QVariantMap result { { "available", false } };
    if (!available() || !std::isfinite(seconds))
        return result;
    int index;
    if (m_info.format == QLatin1String("bif"))
        index = m_bif.frameAt(seconds);
    else {
        const double value = std::floor(std::max(0.0, seconds) * 1000 / m_info.intervalMs);
        if (value > std::numeric_limits<int>::max())
            return result;
        index = int(value);
        if (m_info.thumbnailCount > 0 && index >= m_info.thumbnailCount)
            return result;
    }
    const QSize size
        = m_info.format == QLatin1String("bif") ? m_bif.frameSize(index) : QSize(m_info.width, m_info.height);
    if (index < 0 || !size.isValid() || qint64(size.width()) * size.height() > 1024 * 1024)
        return result;
    if (!authorize(index))
        return result;
    select(index);
    const bool resident = residentSheet();
    const int tile = resident ? index % (m_info.tileWidth * m_info.tileHeight) : 0;
    return { { "available", true }, { "frameIndex", index },
        { "url", QStringLiteral("image://%1/%2/%3").arg(m_providerName).arg(m_generation).arg(textureIndex(index)) },
        { "width", size.width() }, { "height", size.height() },
        { "offsetX", resident ? -(tile % m_info.tileWidth) * size.width() : 0 },
        { "offsetY", resident ? -(tile / m_info.tileWidth) * size.height() : 0 },
        { "sheetWidth", resident ? size.width() * m_info.tileWidth : size.width() },
        { "sheetHeight", resident ? size.height() * m_info.tileHeight : size.height() } };
}

void TrickplayService::select(int index)
{
    if (m_selectedIndex >= 0 && index != m_selectedIndex)
        m_direction = index > m_selectedIndex ? 1 : -1;
    m_selectedIndex = index;
    const int key = textureIndex(index);
    const int anchor = m_info.format == QLatin1String("bif") ? index : index / (m_info.tileWidth * m_info.tileHeight);
    if (anchor != m_prefetchAnchor || m_direction != m_prefetchDirection) {
        m_prefetchAnchor = anchor;
        m_prefetchDirection = m_direction;
        m_prefetchAttempts = 0;
        if (m_reply && m_fetchSpeculative && m_fetchIndex != key)
            abortFetch();
        if (m_decoding && m_decodeSpeculative && m_decodeIndex != key)
            m_decodeCancelled->store(true);
    }
    supersede(key);
    if (m_reply && !m_fetchSpeculative && m_fetchIndex != key)
        abortFetch();
    if (m_pendingIndex >= 0 && m_pendingIndex != key)
        m_pendingIndex = -1;
    prefetch();
}

QQuickImageResponse *TrickplayService::requestImageResponse(const QString& id)
{
    return new TrickplayImageResponse(this, id);
}

void TrickplayService::supersede(int preserveIndex)
{
    for (auto it = m_waiters.begin(); it != m_waiters.end();) {
        if (it.key() == preserveIndex) {
            ++it;
            continue;
        }
        for (const auto& waiter : std::as_const(it.value()))
            if (waiter)
                waiter->finish({}, QStringLiteral("Preview superseded"));
        it = m_waiters.erase(it);
    }
}

void TrickplayService::request(int index, TrickplayImageResponse *response)
{
    if (!available() || index < 0 || index != textureIndex(index)
        || (m_info.format == QLatin1String("bif") && index >= m_bif.count())
        || (m_info.format != QLatin1String("bif") && m_info.thumbnailCount > 0 && index >= m_info.thumbnailCount)) {
        if (response)
            response->finish({}, QStringLiteral("Invalid preview frame"));
        return;
    }
    // Provider requests are queued independently of descriptor selection.
    // A late job must not supersede the newer selection's live delivery.
    if (m_selectedIndex < 0 || index != textureIndex(m_selectedIndex)) {
        if (response)
            response->finish({}, QStringLiteral("Preview superseded"));
        return;
    }
    if (!authorize(index)) {
        if (response)
            response->finish({}, QStringLiteral("Preview account unavailable"));
        return;
    }
    supersede(index);
    if (const auto *image = m_images.object(index)) {
        if (m_reply && !m_fetchSpeculative && m_fetchIndex != index)
            abortFetch();
        m_pendingIndex = -1;
        if (response)
            response->finish(*image, {});
        prefetch();
        return;
    }
    if (m_reply) {
        if (m_fetchIndex == index)
            m_fetchSpeculative = false;
        else
            abortFetch();
    }
    if (response)
        m_waiters[index].append(response);
    if (m_decoding) {
        if (m_decodeIndex == index && !m_decodeCancelled->load()) {
            m_decodeSpeculative = false;
            return;
        }
        if (m_decodeSpeculative)
            m_decodeCancelled->store(true);
        m_pendingIndex = index;
        return;
    }
    startDecode(index);
}

void TrickplayService::abortFetch()
{
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
    }
    m_reply.clear();
    m_fetchIndex = -1;
    m_fetchBytes.clear();
    m_fetchUrl.clear();
}

void TrickplayService::fetch(int index, bool speculative)
{
    if (!m_enabled)
        return;
    const QString scoped = resourceUrl(index);
    if (m_reply && m_fetchUrl == scoped) {
        if (!speculative) {
            m_fetchSpeculative = false;
            m_fetchIndex = index;
        }
        return;
    }
    abortFetch();
    const auto resource = m_source ? m_source->resolveImage(QUrl(scoped)) : ArtworkSource::ImageResource {};
    if (!resource.url.isValid() || resource.url.scheme().isEmpty()) {
        clear();
        return;
    }
    QNetworkRequest request(resource.url);
    request.setTransferTimeout(15000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    for (const auto& line : resource.headers.split('\n')) {
        const auto colon = line.indexOf(':');
        if (colon > 0)
            request.setRawHeader(line.first(colon), line.mid(colon + 1).trimmed());
    }
    m_fetchUrl = scoped;
    m_fetchIndex = index;
    m_fetchSpeculative = speculative;
    QNetworkReply *reply = m_network->get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (reply->bytesAvailable() > MaximumBytes - m_fetchBytes.size()) {
            reply->abort();
            return;
        }
        m_fetchBytes += reply->readAll();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        m_reply.clear();
        const int index = std::exchange(m_fetchIndex, -1);
        const bool speculative = m_fetchSpeculative;
        QString error = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
        if (reply->bytesAvailable() > MaximumBytes - m_fetchBytes.size())
            error = QStringLiteral("Preview exceeds byte budget");
        else
            m_fetchBytes += reply->readAll();
        reply->deleteLater();
        QByteArray bytes = std::move(m_fetchBytes);
        m_fetchUrl.clear();
        if (!authorize(index))
            return;
        if (!error.isEmpty()) {
            if (providerLogEnabled(ProviderLogLevel::Trace))
                writeProviderLog(ProviderLogLevel::Trace,
                    QStringLiteral("preview fetch failed networkCode=%1 status=%2 frame=%3 speculative=%4")
                        .arg(int(reply->error()))
                        .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
                        .arg(index)
                        .arg(speculative));
            complete(index, {}, error, speculative);
            prefetch();
            return;
        }
        if (m_info.format == QLatin1String("bif")) {
            if (!m_bif.parse(std::move(bytes), &error)) {
                complete(index, {}, error, false);
                return;
            }
            const int selected = m_bif.frameAt(m_startSeconds);
            const QSize size = m_bif.frameSize(selected);
            if (!size.isValid() || qint64(size.width()) * size.height() > 1024 * 1024) {
                complete(index, {}, QStringLiteral("Invalid BIF frame dimensions"), false);
                return;
            }
            m_info.width = size.width();
            m_info.height = size.height();
            select(selected);
            startDecode(selected);
            emit changed();
        } else {
            const int sheet = index / (m_info.tileWidth * m_info.tileHeight);
            m_sheets.insert(sheet, new QByteArray(bytes), int(bytes.size()));
            if (speculative) {
                const qint64 cost
                    = trickplayTextureCost(QSize(m_info.width * m_info.tileWidth, m_info.height * m_info.tileHeight));
                if (residentSheet() && !m_decoding && m_images.totalCost() + cost <= TrickplayDecodedByteBudget)
                    decode(index, std::move(bytes), true);
            } else if (m_decoding)
                m_pendingIndex = index;
            else
                decode(index, std::move(bytes));
        }
    });
}

void TrickplayService::startDecode(int index, bool speculative)
{
    if (m_info.format == QLatin1String("bif")) {
        qsizetype offset = 0, length = 0;
        QByteArray backing = m_bif.frameBacking(index, &offset, &length);
        decode(index, std::move(backing), speculative, offset, length);
    } else {
        const int sheet = index / (m_info.tileWidth * m_info.tileHeight);
        if (const QByteArray *bytes = m_sheets.object(sheet))
            decode(index, *bytes, speculative);
        else
            fetch(index, speculative);
    }
}

void TrickplayService::decode(int index, QByteArray bytes, bool speculative, qsizetype offset, qsizetype length)
{
    m_decoding = true;
    m_decodeSpeculative = speculative;
    m_decodeIndex = index;
    m_pendingIndex = -1;
    m_decodeCancelled = std::make_shared<std::atomic_bool>(false);
    const bool bif = m_info.format == QLatin1String("bif");
    const bool resident = residentSheet();
    const QSize expected
        = bif ? m_bif.frameSize(index) : QSize(m_info.width * m_info.tileWidth, m_info.height * m_info.tileHeight);
    QRect crop;
    if (!bif) {
        const int tile = index % (m_info.tileWidth * m_info.tileHeight);
        crop = QRect((tile % m_info.tileWidth) * m_info.width, (tile / m_info.tileWidth) * m_info.height, m_info.width,
            m_info.height);
    }
    const QSize outputSize = resident || bif ? expected : crop.size();
    const qint64 cost = trickplayTextureCost(outputSize);
    // Reserve before allocating the foreground image, rather than temporarily
    // retaining a full old cache alongside the new sheet.
    if (!speculative && m_images.totalCost() + cost > TrickplayDecodedByteBudget)
        m_images.clear();
    const quint64 generation = m_generation;
    const auto cancelled = m_cancelled;
    const auto decodeCancelled = m_decodeCancelled;
    const bool accurate = m_accurateDecoding;
    m_pool.start(QRunnable::create([this, generation, cancelled, decodeCancelled, index, crop, expected, resident,
                                       accurate, offset, length, bytes = std::move(bytes)] {
        QString error;
        std::shared_ptr<const TrickplayTexture> image;
        if (!cancelled->load() && !decodeCancelled->load()) {
            const QByteArray view
                = QByteArray::fromRawData(bytes.constData() + offset, length < 0 ? bytes.size() : length);
            image = decodeTrickplayTexture(view, crop, expected, resident, accurate, &error);
        }
        if (cancelled->load())
            return;
        QMetaObject::invokeMethod(
            this,
            [this, generation, decodeCancelled, index, image = std::move(image), error]() mutable {
                if (generation != m_generation)
                    return;
                const bool speculative = m_decodeSpeculative;
                m_decoding = false;
                m_decodeIndex = -1;
                if (!decodeCancelled->load() && authorize(index))
                    complete(index, std::move(image), error, speculative);
                image = {};
                if (generation != m_generation)
                    return;
                const int pending = std::exchange(m_pendingIndex, -1);
                if (pending >= 0)
                    request(pending, nullptr);
                else
                    prefetch();
            },
            Qt::QueuedConnection);
    }));
}

void TrickplayService::prefetch()
{
    if (m_selectedIndex < 0 || !available() || m_reply || m_decoding || m_pendingIndex >= 0
        || !m_images.contains(textureIndex(m_selectedIndex)))
        return;
    const bool bif = m_info.format == QLatin1String("bif");
    const int limit = bif ? 2 : 1;
    while (m_prefetchAttempts < limit) {
        const qint64 neighbour = qint64(m_prefetchAnchor) + qint64(m_direction) * (++m_prefetchAttempts);
        const qint64 index = bif ? neighbour : neighbour * m_info.tileWidth * m_info.tileHeight;
        // Unknown sheet counts cannot safely speculate beyond the descriptor.
        const int count = bif ? m_bif.count() : m_info.thumbnailCount;
        if (index < 0 || count <= 0 || index >= count || index > std::numeric_limits<int>::max())
            return;
        const int key = textureIndex(int(index));
        if (m_images.contains(key))
            continue;
        if (!authorize(key))
            return;
        if (bif) {
            const QSize size = m_bif.frameSize(key);
            const qint64 cost = trickplayTextureCost(size);
            if (size.isValid() && cost > 0 && cost <= 4 * 1024 * 1024
                && m_images.totalCost() + cost <= TrickplayDecodedByteBudget)
                startDecode(key, true);
            return;
        }
        const int sheet = int(neighbour);
        if (m_sheets.contains(sheet)) {
            const qint64 cost
                = trickplayTextureCost(QSize(m_info.width * m_info.tileWidth, m_info.height * m_info.tileHeight));
            if (residentSheet() && m_images.totalCost() + cost <= TrickplayDecodedByteBudget)
                startDecode(key, true);
        } else
            fetch(key, true);
        return;
    }
}

void TrickplayService::complete(
    int index, std::shared_ptr<const TrickplayTexture> image, const QString& error, bool speculative)
{
    if (image) {
        // Charge RGB-equivalent bytes: smaller planes do not expand the cache.
        const qint64 cost = trickplayTextureCost(image->size);
        if (!speculative || m_images.totalCost() + cost <= TrickplayDecodedByteBudget)
            m_images.insert(index, new std::shared_ptr<const TrickplayTexture>(image), int(cost));
    } else if (!error.isEmpty()) {
        if (providerLogEnabled(ProviderLogLevel::Trace))
            writeProviderLog(ProviderLogLevel::Trace,
                QStringLiteral("preview load failed frame=%1 speculative=%2").arg(index).arg(speculative));
        if (!speculative)
            writeProviderLog(ProviderLogLevel::Warn, QStringLiteral("Preview frame unavailable"));
    }
    const auto waiters = m_waiters.take(index);
    for (const auto& waiter : waiters)
        if (waiter)
            waiter->finish(image, error);
}

QQuickImageResponse *TrickplayImageProvider::requestImageResponse(const QString& id, const QSize&)
{
    return m_service ? m_service->requestImageResponse(id) : new TrickplayImageResponse(nullptr, id);
}
} // namespace Spool
