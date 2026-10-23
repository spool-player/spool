#include "SpeedTest.h"

#include "ScriptBridge.h"

#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Spool {

NetworkLaneSelection selectNetworkLanes(const QVariantList& samples, double mediaBitrate, int maxRequests)
{
    double best = 0;
    maxRequests = std::clamp(maxRequests, 1, 16);
    for (const QVariant& value : samples) {
        const auto sample = value.toMap();
        const int requests = sample.value(QStringLiteral("requests")).toInt();
        const double capacity = sample.value(QStringLiteral("capacity-bytes-per-second")).toDouble();
        if (requests >= 1 && requests <= maxRequests && std::isfinite(capacity))
            best = std::max(best, capacity);
    }
    if (!(best > 0))
        return {};
    // Sub-15% gains are not evidence that another socket is useful, especially
    // on fast links where scheduling and connection warmup dominate tiny tests.
    double threshold = best * 0.85;
    int reason = 1;
    if (std::isfinite(mediaBitrate) && mediaBitrate > 0 && mediaBitrate * 1.75 / 8 <= threshold) {
        threshold = mediaBitrate * 1.75 / 8;
        reason = 2;
    }
    int selected = 17;
    for (const QVariant& value : samples) {
        const auto sample = value.toMap();
        const int requests = sample.value(QStringLiteral("requests")).toInt();
        if (requests >= 1 && requests <= maxRequests
            && sample.value(QStringLiteral("capacity-bytes-per-second")).toDouble() >= threshold)
            selected = std::min(selected, requests);
    }
    return { selected <= 16 ? selected : 1, reason };
}

SpeedTest::SpeedTest(ScriptAccess *access, Completion completion, QObject *parent)
    : QObject(parent)
    , m_access(access)
    , m_completion(std::move(completion))
{
    m_roundDeadline.setSingleShot(true);
    connect(&m_roundDeadline, &QTimer::timeout, this, [this] {
        qint64 received = m_round >= 0 ? m_received[m_round] : 0;
        for (int i = 0; i < m_count; ++i) {
            if (m_lanes[i].reply && !drain(i))
                return;
            received += m_lanes[i].received;
        }
        if (received == 0)
            return finish(QStringLiteral("network_error"));
        completeRound();
    });
}

SpeedTest::~SpeedTest()
{
    abort();
}

void SpeedTest::abort()
{
    m_roundDeadline.stop();
    for (Lane& lane : m_lanes) {
        if (!lane.reply)
            continue;
        QNetworkReply *reply = std::exchange(lane.reply, nullptr);
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

void SpeedTest::finish(QString error, qint64 bitrate)
{
    if (m_done)
        return;
    m_done = true;
    abort();
    auto completion = std::move(m_completion);
    QVariantMap result;
    if (error.isEmpty()) {
        QVariantList samples;
        for (int i = 0; i < 3; ++i) {
            samples.append(QVariantMap { { QStringLiteral("requests"), 1 << i },
                { QStringLiteral("capacity-bytes-per-second"), m_rates[i] / 8 },
                { QStringLiteral("ttfb-ms"), m_latencies[i] }, { QStringLiteral("elapsed-seconds"), m_durations[i] },
                { QStringLiteral("received-bytes"), m_received[i] } });
            qInfo("streaming probe sample requests=%d capacity_bytes_per_second=%.0f ttfb_ms=%.3f "
                  "elapsed_seconds=%.3f received_bytes=%lld",
                1 << i, m_rates[i] / 8, m_latencies[i], m_durations[i], static_cast<long long>(m_received[i]));
        }
        const auto selection = selectNetworkLanes(samples, m_targetBitrate);
        const int lanes = selection.requests;
        qInfo("streaming probe selection requests=%d reason=%d media_bitrate=%.0f", lanes, selection.reason,
            m_targetBitrate);
        QUrl endpoint;
        endpoint.setScheme(m_request.url().scheme());
        endpoint.setHost(m_request.url().host());
        endpoint.setPort(m_request.url().port());
        result = { { QStringLiteral("bitrate"), bitrate }, { QStringLiteral("parallelRequests"), lanes },
            { QStringLiteral("concurrency-samples"), samples }, { QStringLiteral("origin"), endpoint.toString() } };
    }
    completion(std::move(error), std::move(result));
}

void SpeedTest::start(const QVariantMap& options)
{
    const double target = options.value(QStringLiteral("mediaBitrate")).toDouble();
    if (std::isfinite(target) && target > 0 && target <= 800.0e9)
        m_targetBitrate = target;
    const QVariant address = options.value(QStringLiteral("url"));
    if (address.metaType().id() != QMetaType::QString)
        return finish(QStringLiteral("request_denied"));
    m_template = address.toString();
    const QVariant range = options.value(QStringLiteral("range"));
    if (range.isValid() && range.metaType().id() != QMetaType::Bool)
        return finish(QStringLiteral("request_denied"));
    m_range = range.toBool();
    if (m_template.size() > 8192
        || (!m_range
            && (!m_template.contains(QStringLiteral("{bytes}")) || !m_template.contains(QStringLiteral("{nonce}")))))
        return finish(QStringLiteral("request_denied"));
    m_nonce = QUuid::createUuid().toString(QUuid::Id128);
    QString probe = m_template;
    probe.replace(QStringLiteral("{bytes}"), QStringLiteral("524288"));
    probe.replace(QStringLiteral("{nonce}"), m_nonce);
    const QUrl url(probe, QUrl::StrictMode);
    const qsizetype authorityStart = m_template.indexOf(QStringLiteral("://")) + 3;
    qsizetype authorityEnd = m_template.size();
    for (const QChar delimiter : { QLatin1Char('/'), QLatin1Char('?'), QLatin1Char('#') }) {
        const qsizetype position = m_template.indexOf(delimiter, authorityStart);
        if (position >= 0)
            authorityEnd = std::min(authorityEnd, position);
    }
    const QString authority = m_template.mid(authorityStart, authorityEnd - authorityStart);
    if ((url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")) || !m_access->allows(url)
        || authority.contains(QLatin1Char('{')) || url.hasFragment())
        return finish(QStringLiteral("request_denied"));
    m_request = QNetworkRequest(url);
    m_request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    m_request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    m_request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    m_request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    m_request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    m_request.setTransferTimeout(10000);
    const QVariant rawHeaders = options.value(QStringLiteral("headers"));
    if (rawHeaders.isValid() && rawHeaders.metaType().id() != QMetaType::QVariantMap)
        return finish(QStringLiteral("header_denied"));
    const QVariantMap headers = rawHeaders.toMap();
    qsizetype headerBytes = 0;
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        const QByteArray name = it.key().toLatin1();
        if (it.value().metaType().id() != QMetaType::QString)
            return finish(QStringLiteral("header_denied"));
        const QByteArray value = it.value().toString().toUtf8();
        headerBytes += name.size() + value.size();
        const bool token = !name.isEmpty() && std::all_of(name.cbegin(), name.cend(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                || QByteArrayView("!#$%&'*+-.^_`|~").contains(c);
        });
        if (!token || QString::fromLatin1(name) != it.key() || headerBytes > 64 * 1024 || value.contains('\r')
            || value.contains('\n') || value.contains('\0') || name.compare("host", Qt::CaseInsensitive) == 0
            || name.compare("content-length", Qt::CaseInsensitive) == 0
            || name.compare("transfer-encoding", Qt::CaseInsensitive) == 0
            || name.compare("accept-encoding", Qt::CaseInsensitive) == 0
            || name.compare("range", Qt::CaseInsensitive) == 0)
            return finish(QStringLiteral("header_denied"));
        m_request.setRawHeader(name, value);
    }
    // Compression measures generated/compressed bytes rather than link bandwidth.
    m_request.setRawHeader("Accept-Encoding", "identity");
    m_request.setRawHeader("Cache-Control", "no-cache, no-store");
    round(1, 512 * 1024);
}

void SpeedTest::round(int lanes, qint64 totalBytes)
{
    m_count = lanes;
    m_remaining = lanes;
    m_expected = totalBytes / lanes;
    m_elapsed.start();
    if (m_round < 0 || m_received[m_round] == 0)
        m_pointElapsed.start();
    // Each operating point gets at most 2.5 s (including repeated chunks),
    // leaving room for warmup and JS completion inside the operation's 15 s.
    m_roundDeadline.start(std::max(1, 2500 - int(m_pointElapsed.elapsed())));
    ++m_transfer;
    for (int i = 0; i < lanes; ++i) {
        QString address = m_template;
        address.replace(QStringLiteral("{bytes}"), QString::number(m_expected));
        address.replace(QStringLiteral("{nonce}"),
            m_nonce + QLatin1Char('-') + QString::number(m_transfer) + QLatin1Char('-') + QString::number(i));
        const QUrl url(address, QUrl::StrictMode);
        if ((url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))
            || !m_access->allows(url))
            return finish(QStringLiteral("request_denied"));
        m_request.setUrl(url);
        if (m_range) {
            const qint64 offset = i * m_expected;
            m_request.setRawHeader(
                "Range", "bytes=" + QByteArray::number(offset) + '-' + QByteArray::number(offset + m_expected - 1));
        }
        QNetworkReply *reply = m_access->network->get(m_request);
        reply->setReadBufferSize(m_buffer.size());
        m_lanes[i] = { reply, 0 };
        connect(reply, &QNetworkReply::metaDataChanged, this, [this, i] {
            m_lanes[i].responseChecked = false;
            checkResponse(i);
        });
        connect(reply, &QIODevice::readyRead, this, [this, i] { drain(i); });
        connect(reply, &QNetworkReply::finished, this, [this, i] { finished(i); });
    }
}

bool SpeedTest::checkResponse(int index)
{
    Lane& lane = m_lanes[index];
    if (m_done || !lane.reply)
        return false;
    if (lane.responseChecked)
        return true;
    QNetworkReply *reply = lane.reply;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 0 && status != (m_range ? 206 : 200)) {
        finish(QStringLiteral("http_%1").arg(status));
        return false;
    }
    if (m_range && status == 206) {
        // Validate the server honored this exact range, not merely its length.
        const QByteArray requested = reply->request().rawHeader("Range").mid(6);
        const QByteArray contentRange = reply->rawHeader("Content-Range");
        const QByteArray prefix = "bytes " + requested + '/';
        bool validTotal = false;
        const qint64 total = contentRange.mid(prefix.size()).toLongLong(&validTotal);
        const qint64 end = requested.mid(requested.indexOf('-') + 1).toLongLong();
        if (!contentRange.startsWith(prefix) || !validTotal || total <= end) {
            finish(QStringLiteral("invalid_sample"));
            return false;
        }
    }
    const QByteArray encoding = reply->rawHeader("Content-Encoding");
    if ((!encoding.isEmpty() && encoding.compare("identity", Qt::CaseInsensitive) != 0)
        || (reply->hasRawHeader("Content-Length") && reply->rawHeader("Content-Length").toLongLong() != m_expected)) {
        finish(QStringLiteral("invalid_sample"));
        return false;
    }
    lane.responseChecked = status != 0;
    return true;
}

bool SpeedTest::drain(int index)
{
    Lane& lane = m_lanes[index];
    if (!checkResponse(index))
        return false;
    while (lane.reply->bytesAvailable() > 0) {
        const qint64 bytes = lane.reply->read(m_buffer.data(), m_buffer.size());
        if (bytes <= 0)
            break;
        if (lane.firstByteNs < 0)
            lane.firstByteNs = m_elapsed.nsecsElapsed();
        lane.received += bytes;
        if (lane.received > m_expected) {
            finish(QStringLiteral("response_limit"));
            return false;
        }
    }
    return true;
}

void SpeedTest::finished(int index)
{
    if (!drain(index))
        return;
    Lane& lane = m_lanes[index];
    QNetworkReply *reply = lane.reply;
    if (reply->error() != QNetworkReply::NoError
        || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != (m_range ? 206 : 200))
        return finish(QStringLiteral("network_error"));
    if (lane.received != m_expected)
        return finish(QStringLiteral("invalid_sample"));
    disconnect(reply, nullptr, this, nullptr);
    reply->deleteLater();
    lane.reply = nullptr;
    if (--m_remaining != 0)
        return;
    completeRound();
}

void SpeedTest::completeRound()
{
    const qint64 nanoseconds = std::max<qint64>(1, m_elapsed.nsecsElapsed());
    qint64 received = 0;
    double latency = 0;
    for (int i = 0; i < m_count; ++i) {
        received += m_lanes[i].received;
        latency += (m_lanes[i].firstByteNs >= 0 ? m_lanes[i].firstByteNs : nanoseconds) / 1.0e6;
    }
    abort();
    if (m_round >= 0) {
        const double previousDuration = m_durations[m_round];
        m_received[m_round] += received;
        m_durations[m_round] += nanoseconds / 1.0e9;
        m_latencies[m_round] = (m_latencies[m_round] * previousDuration + latency / m_count * (nanoseconds / 1.0e9))
            / m_durations[m_round];
        m_rates[m_round] = m_received[m_round] * 8.0 / m_durations[m_round];
        // Short transfers get more wall time instead of rewarding a single
        // millisecond-scale result. Bound both traffic and duration; Range
        // probes repeat the minimum-size resource rather than reading past it.
        if (m_durations[m_round] < 0.4 && m_received[m_round] < 64 * 1024 * 1024 && m_pointElapsed.elapsed() < 2500)
            return round(m_count, std::min(m_chunkBytes, 64LL * 1024 * 1024 - m_received[m_round]));
    } else {
        const double predicted = received * 0.4e9 / nanoseconds;
        // Power-of-two batches fit common server buffer size classes, while
        // each lane still gets exactly the requested bounded payload.
        m_chunkBytes = 4 * 1024 * 1024;
        if (!m_range) {
            while (m_chunkBytes < 32 * 1024 * 1024 && m_chunkBytes * 2 <= predicted)
                m_chunkBytes *= 2;
        }
    }
    // Always measure the complete curve, including negative scaling.
    if (++m_round < 3)
        return round(1 << m_round, m_chunkBytes);
    const qint64 bitrate = std::llround(*std::max_element(m_rates.cbegin(), m_rates.cend()) * 0.75);
    finish({}, bitrate);
}

} // namespace Spool
