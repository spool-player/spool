#include "RangedDownload.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>

#include <algorithm>

namespace Spool {

namespace {
    constexpr qint64 kReadBufferBytes = 256 * 1024;
    // A dropped connection resumes from where its lane got to.
    constexpr int kLaneRetries = 2;
} // namespace

RangedDownload::RangedDownload(QNetworkAccessManager *network, QNetworkRequest request, QString path, QObject *parent)
    : QObject(parent)
    , m_network(network)
    , m_request(std::move(request))
    , m_file(std::move(path))
{
}

RangedDownload::~RangedDownload()
{
    abort();
}

void RangedDownload::setLanes(int lanes)
{
    m_laneLimit = std::max(1, lanes);
}

void RangedDownload::setExpectedBytes(qint64 bytes)
{
    m_expected = bytes;
}

void RangedDownload::setMinimumLaneBytes(qint64 bytes)
{
    m_minimumLaneBytes = std::max<qint64>(1, bytes);
}

void RangedDownload::setPrepareReply(PrepareReply prepare)
{
    m_prepare = std::move(prepare);
}

qint64 RangedDownload::receivedBytes() const
{
    return m_received;
}

qint64 RangedDownload::totalBytes() const
{
    return m_total;
}

void RangedDownload::start()
{
    if (m_running)
        return;
    m_running = true;
    m_ranged = true;
    m_received = 0;
    m_total = -1;
    if (!m_file.open(QIODevice::ReadWrite | QIODevice::Truncate)) {
        fail(QStringLiteral("The download could not be written."));
        return;
    }
    if (m_expected == 0) {
        m_total = 0;
        succeed();
        return;
    }
    auto first = std::make_unique<Lane>();
    Lane *lane = first.get();
    m_lanes.push_back(std::move(first));
    // A known size is split before anything is asked; otherwise the first
    // answer says how big the file is and the split happens then.
    if (m_expected > 0) {
        if (!setTotal(m_expected))
            return;
        lane->end = m_total;
        rebalance();
    }
    request(lane);
}

void RangedDownload::abort()
{
    m_running = false;
    stopLanes();
    m_file.close();
}

void RangedDownload::request(Lane *lane)
{
    QNetworkRequest request = m_request;
    // Over HTTP/2 every lane would share one connection, which is the very
    // limit this is here to get past.
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    // Ranges count stored bytes; a compressed response would count others.
    request.setRawHeader("Accept-Encoding", "identity");
    if (m_ranged) {
        QByteArray range = "bytes=" + QByteArray::number(lane->next) + '-';
        if (lane->end > 0)
            range += QByteArray::number(lane->end - 1);
        request.setRawHeader("Range", range);
    }
    lane->checked = false;
    QNetworkReply *reply = m_network->get(request);
    lane->reply = reply;
    reply->setReadBufferSize(kReadBufferBytes);
    if (m_prepare)
        m_prepare(reply);
    connect(reply, &QNetworkReply::readyRead, this, [this, lane] { drain(lane); });
    connect(reply, &QNetworkReply::finished, this, [this, lane] { laneFinished(lane); });
}

// False when the lane is gone: failed, or replaced by a plain download.
bool RangedDownload::check(Lane *lane)
{
    if (lane->checked)
        return true;
    QNetworkReply *reply = lane->reply;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 0)
        return true;
    const QByteArray encoding = reply->rawHeader("Content-Encoding");
    if (!encoding.isEmpty() && encoding.compare("identity", Qt::CaseInsensitive) != 0) {
        fail(QStringLiteral("The server compressed the download."));
        return false;
    }
    if (m_ranged && status == 206) {
        // "bytes first-last/total"
        const QByteArray range = reply->rawHeader("Content-Range");
        const qsizetype dash = range.indexOf('-');
        const qsizetype slash = range.indexOf('/');
        bool firstValid = false;
        bool totalValid = false;
        const qint64 first = range.mid(6, dash - 6).toLongLong(&firstValid);
        const qint64 total = range.mid(slash + 1).toLongLong(&totalValid);
        if (!range.startsWith("bytes ") || dash < 0 || slash < dash || !firstValid || !totalValid || first != lane->next
            || total <= first) {
            fail(QStringLiteral("The server sent the wrong part of the file."));
            return false;
        }
        if (m_total < 0) {
            if (!setTotal(total))
                return false;
            lane->end = total;
            lane->checked = true;
            rebalance();
            return true;
        }
        if (total != m_total) {
            fail(QStringLiteral("The file changed size during the download."));
            return false;
        }
        lane->checked = true;
        return true;
    }
    if (status != 200) {
        fail(QStringLiteral("HTTP %1").arg(status));
        return false;
    }
    // The server ignored the range and is sending the whole file. That will
    // do when this is the only lane and it starts at the beginning; anything
    // else starts over as one plain download.
    if (m_ranged && (lane->next != 0 || m_lanes.size() != 1)) {
        restartWhole();
        return false;
    }
    m_ranged = false;
    const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
    if (length.isValid()) {
        if (m_total < 0 && !setTotal(length.toLongLong()))
            return false;
        if (length.toLongLong() != m_total) {
            fail(QStringLiteral("The server's file is not the expected size."));
            return false;
        }
    }
    lane->end = m_total;
    lane->checked = true;
    return true;
}

// False when the lane is gone, or the download stopped while it was read.
bool RangedDownload::drain(Lane *lane)
{
    if (!check(lane))
        return false;
    QNetworkReply *reply = lane->reply;
    bool advanced = false;
    while (reply->bytesAvailable() > 0) {
        const QByteArray bytes = reply->read(kReadBufferBytes);
        if (bytes.isEmpty())
            break;
        qint64 length = bytes.size();
        if (lane->end >= 0 && length > lane->end - lane->next) {
            // Past a split point belongs to the lane that took it over; past
            // the end of a plain download is a server sending too much.
            if (!m_ranged) {
                fail(QStringLiteral("The server sent more than the file."));
                return false;
            }
            length = lane->end - lane->next;
        }
        if (!m_file.seek(lane->next) || m_file.write(bytes.constData(), length) != length) {
            fail(QStringLiteral("The download could not be written."));
            return false;
        }
        lane->next += length;
        m_received += length;
        advanced = true;
        if (lane->end >= 0 && lane->next >= lane->end) {
            emit progress();
            if (m_running)
                completeLane(lane);
            return false;
        }
    }
    if (advanced)
        emit progress();
    return m_running;
}

void RangedDownload::laneFinished(Lane *lane)
{
    if (!drain(lane))
        return;
    QNetworkReply *reply = lane->reply;
    const bool clean = reply->error() == QNetworkReply::NoError;
    const QString message = reply->errorString();
    disconnect(reply, nullptr, this, nullptr);
    reply->deleteLater();
    lane->reply = nullptr;
    // A plain download of unknown size is whole when the server stops.
    if (clean && lane->end < 0) {
        m_total = lane->next;
        completeLane(lane);
        return;
    }
    if (m_ranged && lane->retries++ < kLaneRetries) {
        request(lane);
        return;
    }
    fail(clean ? QStringLiteral("The download ended early.") : message);
}

void RangedDownload::completeLane(Lane *lane)
{
    if (QNetworkReply *reply = lane->reply) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    std::erase_if(m_lanes, [lane](const auto& candidate) { return candidate.get() == lane; });
    if (m_lanes.empty())
        succeed();
    else
        rebalance();
}

// Fills free lanes by halving whichever lane has the most left.
void RangedDownload::rebalance()
{
    if (!m_ranged || m_total < 0)
        return;
    while (static_cast<int>(m_lanes.size()) < m_laneLimit) {
        Lane *widest = nullptr;
        for (const auto& lane : m_lanes) {
            if (!widest || lane->end - lane->next > widest->end - widest->next)
                widest = lane.get();
        }
        if (!widest || widest->end - widest->next < 2 * m_minimumLaneBytes)
            return;
        auto lane = std::make_unique<Lane>();
        lane->next = widest->next + (widest->end - widest->next) / 2;
        lane->end = widest->end;
        // The widest lane's request still runs to its old end; it stops
        // reading here instead.
        widest->end = lane->next;
        Lane *added = lane.get();
        m_lanes.push_back(std::move(lane));
        request(added);
    }
}

void RangedDownload::restartWhole()
{
    stopLanes();
    m_ranged = false;
    m_received = 0;
    auto whole = std::make_unique<Lane>();
    Lane *lane = whole.get();
    lane->end = m_total;
    m_lanes.push_back(std::move(whole));
    emit progress();
    if (m_running)
        request(lane);
}

bool RangedDownload::setTotal(qint64 total)
{
    if (m_expected >= 0 && total != m_expected) {
        fail(QStringLiteral("The server's file is not the expected size."));
        return false;
    }
    m_total = total;
    if (!m_file.resize(total)) {
        fail(QStringLiteral("The download could not be written."));
        return false;
    }
    return true;
}

void RangedDownload::succeed()
{
    m_running = false;
    stopLanes();
    m_file.close();
    emit finished({});
}

void RangedDownload::fail(const QString& error)
{
    if (!m_running)
        return;
    m_running = false;
    stopLanes();
    m_file.close();
    emit finished(error);
}

void RangedDownload::stopLanes()
{
    // Disconnect before abort: an aborted reply finishes on the spot.
    for (const auto& lane : m_lanes) {
        if (QNetworkReply *reply = lane->reply) {
            disconnect(reply, nullptr, this, nullptr);
            reply->abort();
            reply->deleteLater();
        }
    }
    m_lanes.clear();
}

} // namespace Spool
