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

namespace {

    bool altSvcAdvertisesH3(const QByteArray& value)
    {
        if (value.isEmpty())
            return false;
        const QByteArray lower = value.toLower();
        return lower.contains("h3=") || lower.contains("h3-29=");
    }

} // namespace

SpeedTest::SpeedTest(ScriptAccess *access, Completion completion, QObject *parent)
    : QObject(parent)
    , m_access(access)
    , m_completion(std::move(completion))
{
    m_roundDeadline.setSingleShot(true);
    connect(&m_roundDeadline, &QTimer::timeout, this, [this] {
        qint64 received = (m_round >= 0 && m_round < static_cast<int>(m_samples.size()))
            ? m_samples[static_cast<size_t>(m_round)].receivedBytes
            : 0;
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
        double maxRate = 0.0;
        int bestHttpVersion = 0;
        for (const RoundSample& s : m_samples) {
            if (s.rateBps > maxRate) {
                maxRate = s.rateBps;
                bestHttpVersion = s.httpVersion;
            }
            samples.append(QVariantMap { { QStringLiteral("requests"), s.requests },
                { QStringLiteral("http-version"), s.httpVersion },
                { QStringLiteral("capacity-bytes-per-second"), s.rateBps / 8.0 },
                { QStringLiteral("ttfb-ms"), s.ttfbMs }, { QStringLiteral("elapsed-seconds"), s.durationS },
                { QStringLiteral("received-bytes"), s.receivedBytes } });
            qInfo("streaming probe sample requests=%d http=%d capacity_bytes_per_second=%.0f ttfb_ms=%.3f "
                  "elapsed_seconds=%.3f received_bytes=%lld",
                s.requests, s.httpVersion, s.rateBps / 8.0, s.ttfbMs, s.durationS,
                static_cast<long long>(s.receivedBytes));
        }
        // This count describes the offline probe only; native playback chooses
        // its own profile from the complete measured curve.
        const double threshold = maxRate * 0.85;
        int lanes = 1;
        for (const RoundSample& s : m_samples) {
            if (s.rateBps >= threshold) {
                lanes = s.requests;
                break;
            }
        }
        qInfo("streaming probe selection requests=%d http=%d h3_advertised=%d", lanes, bestHttpVersion,
            m_h3Advertised ? 1 : 0);
        QUrl endpoint;
        endpoint.setScheme(m_request.url().scheme());
        endpoint.setHost(m_request.url().host());
        endpoint.setPort(m_request.url().port());
        result = { { QStringLiteral("bitrate"), bitrate }, { QStringLiteral("parallelRequests"), lanes },
            { QStringLiteral("http-version"), bestHttpVersion }, { QStringLiteral("h3-advertised"), m_h3Advertised },
            { QStringLiteral("concurrency-samples"), samples }, { QStringLiteral("origin"), endpoint.toString() } };
    }
    completion(std::move(error), std::move(result));
}

void SpeedTest::start(const QVariantMap& options)
{
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
    m_schedule = { { 1, false }, { 2, false }, { 4, false } };
    m_samples.clear();
    m_samples.reserve(5);
    m_totalElapsed.start();
    round(1, 512 * 1024, false);
}

void SpeedTest::round(int lanes, qint64 totalBytes, bool forceHttp1)
{
    m_count = std::clamp(lanes, 1, static_cast<int>(m_lanes.size()));
    m_remaining = m_count;
    m_expected = totalBytes / m_count;
    m_forceHttp1 = forceHttp1;
    m_elapsed.start();
    const bool continuing = m_round >= 0 && m_round < static_cast<int>(m_samples.size())
        && m_samples[static_cast<size_t>(m_round)].receivedBytes > 0;
    if (!continuing)
        m_pointElapsed.start();
    // Each operating point gets at most 2.5 s (including repeated chunks),
    // leaving room for warmup, optional WAN probes, and JS completion inside 15 s.
    m_roundDeadline.start(std::max(1, 2500 - int(m_pointElapsed.elapsed())));
    ++m_transfer;
    for (int i = 0; i < m_count; ++i) {
        QString address = m_template;
        address.replace(QStringLiteral("{bytes}"), QString::number(m_expected));
        address.replace(QStringLiteral("{nonce}"),
            m_nonce + QLatin1Char('-') + QString::number(m_transfer) + QLatin1Char('-') + QString::number(i));
        const QUrl url(address, QUrl::StrictMode);
        if ((url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))
            || !m_access->allows(url))
            return finish(QStringLiteral("request_denied"));
        QNetworkRequest request = m_request;
        request.setUrl(url);
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, !forceHttp1);
        if (m_range) {
            const qint64 offset = i * m_expected;
            request.setRawHeader(
                "Range", "bytes=" + QByteArray::number(offset) + '-' + QByteArray::number(offset + m_expected - 1));
        }
        QNetworkReply *reply = m_access->network->get(request);
        reply->setReadBufferSize(m_buffer.size());
        m_lanes[i] = { reply, 0, false, -1, 0 };
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
    if (status != 0) {
        const bool usedH2 = reply->attribute(QNetworkRequest::Http2WasUsedAttribute).toBool();
        lane.httpVersion = usedH2 ? 20 : 11;
        if (reply->hasRawHeader("Alt-Svc") && altSvcAdvertisesH3(reply->rawHeader("Alt-Svc")))
            m_h3Advertised = true;
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
    double latency = 0.0;
    int httpVersion = m_forceHttp1 ? 11 : 0;
    for (int i = 0; i < m_count; ++i) {
        received += m_lanes[i].received;
        latency += (m_lanes[i].firstByteNs >= 0 ? m_lanes[i].firstByteNs : nanoseconds) / 1.0e6;
        if (m_lanes[i].httpVersion > 0)
            httpVersion = m_lanes[i].httpVersion;
    }
    abort();
    if (m_round >= 0) {
        RoundSample& sample = m_samples[static_cast<size_t>(m_round)];
        const double previousDuration = sample.durationS;
        sample.receivedBytes += received;
        sample.durationS += nanoseconds / 1.0e9;
        sample.ttfbMs
            = (sample.ttfbMs * previousDuration + latency / m_count * (nanoseconds / 1.0e9)) / sample.durationS;
        sample.rateBps = sample.receivedBytes * 8.0 / sample.durationS;
        if (httpVersion > 0)
            sample.httpVersion = httpVersion;
        // Short transfers get more wall time instead of rewarding a single
        // millisecond-scale result. Bound both traffic and duration; Range
        // probes repeat the minimum-size resource rather than reading past it.
        if (sample.durationS < 0.4 && sample.receivedBytes < 64 * 1024 * 1024 && m_pointElapsed.elapsed() < 2500)
            return round(m_count, std::min(m_chunkBytes, 64LL * 1024 * 1024 - sample.receivedBytes), m_forceHttp1);

        // After the 1/2/4-lane baseline curve finishes, probe higher WAN
        // concurrency or escape an HTTPS HTTP/2 single-socket bottleneck when
        // evidence warrants it and ample wall-clock budget remains.
        if (m_round == 2 && m_samples.size() >= 3) {
            const double ttfb0 = m_samples[0].ttfbMs;
            const bool https = m_request.url().scheme() == QStringLiteral("https");
            const bool h2Stalled = https && m_samples[2].httpVersion == 20 && ttfb0 >= 40.0
                && m_samples[2].rateBps < m_samples[0].rateBps * 1.35;
            if (h2Stalled && !m_addedHttp1Round && m_totalElapsed.elapsed() < 9500) {
                m_addedHttp1Round = true;
                m_schedule.push_back({ 4, true });
            } else if (!m_addedWideRound && ttfb0 >= 60.0 && m_samples[2].rateBps >= m_samples[1].rateBps * 1.25
                && m_totalElapsed.elapsed() < 8500) {
                m_addedWideRound = true;
                m_schedule.push_back({ 8, false });
            }
        }
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
    if (++m_round < static_cast<int>(m_schedule.size())) {
        const RoundSpec spec = m_schedule[static_cast<size_t>(m_round)];
        m_samples.push_back(RoundSample { spec.lanes, spec.forceHttp1 ? 11 : 0, 0.0, 0.0, 0.0, 0 });
        return round(spec.lanes, m_chunkBytes, spec.forceHttp1);
    }
    double maxRate = 0.0;
    for (const RoundSample& s : m_samples)
        maxRate = std::max(maxRate, s.rateBps);
    const qint64 bitrate = std::llround(maxRate * 0.75);
    finish({}, bitrate);
}

} // namespace Spool
