#include "LanProbe.h"

#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Spool {
namespace {
    bool privateAddress(quint32 address)
    {
        return (address & 0xff000000u) == 0x0a000000u || (address & 0xfff00000u) == 0xac100000u
            || (address & 0xffff0000u) == 0xc0a80000u || (address & 0xffff0000u) == 0xa9fe0000u;
    }

    int priority(const LanNetwork& network)
    {
        static const QStringList synthetic { QStringLiteral("docker"), QStringLiteral("br-"), QStringLiteral("virbr"),
            QStringLiteral("veth"), QStringLiteral("vmnet"), QStringLiteral("vboxnet"), QStringLiteral("tun"),
            QStringLiteral("tap"), QStringLiteral("utun"), QStringLiteral("wg"), QStringLiteral("zt"),
            QStringLiteral("tailscale"), QStringLiteral("podman"), QStringLiteral("cni"), QStringLiteral("flannel"),
            QStringLiteral("kube") };
        int score = 0;
        for (const auto& prefix : synthetic) {
            if (network.name.startsWith(prefix, Qt::CaseInsensitive)) {
                score += 100;
                break;
            }
        }
        if (network.flags & QNetworkInterface::IsPointToPoint)
            score += 50;
        if (network.prefixLength < 24)
            score += 10;
        return score;
    }

    bool integer(const QVariant& value, int low, int high)
    {
        bool ok = false;
        const double number = value.toDouble(&ok);
        return ok && value.metaType().id() != QMetaType::QString && value.metaType().id() != QMetaType::Bool
            && std::isfinite(number) && number == std::floor(number) && number >= low && number <= high;
    }
} // namespace

QList<QHostAddress> lanProbeTargets(QList<LanNetwork> networks)
{
    std::stable_sort(
        networks.begin(), networks.end(), [](const auto& a, const auto& b) { return priority(a) < priority(b); });
    QList<QHostAddress> targets;
    QSet<quint32> seen;
    QSet<QString> scanned;
    int count = 0;
    for (const auto& network : networks) {
        if (!(network.flags & QNetworkInterface::IsUp) || !(network.flags & QNetworkInterface::IsRunning)
            || (network.flags & QNetworkInterface::IsLoopBack)
            || network.address.protocol() != QAbstractSocket::IPv4Protocol || network.prefixLength < 1
            || network.prefixLength > 32)
            continue;
        const quint32 own = network.address.toIPv4Address();
        if (!privateAddress(own))
            continue;
        const quint32 mask = 0xffffffffu << (32 - network.prefixLength);
        const quint32 base = own & mask;
        const QString key = QString::number(base) + QLatin1Char('/') + QString::number(network.prefixLength);
        if (scanned.contains(key))
            continue;
        if (count++ == 2)
            break;
        scanned.insert(key);
        int added = 0;
        const auto append = [&](quint32 address) {
            if (added >= 254 || !privateAddress(address) || seen.contains(address))
                return;
            seen.insert(address);
            targets.append(QHostAddress(address));
            ++added;
        };
        append(own);
        // For wide networks scan the local /24 only, never all of a /8 or /16.
        const quint32 start = std::max(base, own & 0xffffff00u);
        const quint32 end = std::min(base | ~mask, (own & 0xffffff00u) | 0xffu);
        if (network.prefixLength <= 30) {
            append(start + 1);
            if (own > start + 1)
                append(own - 1);
            if (own + 1 < end)
                append(own + 1);
            for (quint32 address = start + 1; address < end && added < 254; ++address)
                append(address);
        }
    }
    if (!targets.isEmpty())
        targets.prepend(QHostAddress(QHostAddress::LocalHost));
    return targets;
}

QList<QHostAddress> localLanProbeTargets()
{
    QList<LanNetwork> networks;
    for (const auto& interface : QNetworkInterface::allInterfaces()) {
        for (const auto& entry : interface.addressEntries())
            networks.append({ interface.name(), interface.flags(), entry.ip(), entry.prefixLength() });
    }
    return lanProbeTargets(std::move(networks));
}

LanProbe::LanProbe(std::shared_ptr<LanProbeSession> session, Completion completion, QObject *parent)
    : QObject(parent)
    , m_session(std::move(session))
    , m_completion(std::move(completion))
{
    m_network.setProxy(QNetworkProxy::NoProxy);
    m_deadline.setSingleShot(true);
    m_deadline.setTimerType(Qt::PreciseTimer);
    connect(&m_deadline, &QTimer::timeout, this, [this] { finish(); });
}

LanProbe::~LanProbe()
{
    abortReplies();
    if (m_session->active == this) {
        m_session->active.clear();
        m_session->cursor.clear();
        m_session->snapshot.clear();
    }
}

void LanProbe::start(const QVariantMap& options)
{
    const QVariant port = options.value(QStringLiteral("port"));
    const QVariant limit = options.value(QStringLiteral("limit"), 32);
    const QString path = options.value(QStringLiteral("path")).toString();
    const QUrl parsed(path, QUrl::StrictMode);
    if (!integer(port, 1, 65535) || !integer(limit, 1, 32) || path.size() > 2048 || !path.startsWith(QLatin1Char('/'))
        || path.startsWith(QStringLiteral("//")) || !parsed.isValid() || !parsed.isRelative() || parsed.hasQuery()
        || parsed.hasFragment() || path.contains(QLatin1Char('\\')) || path.contains(QLatin1Char('\r'))
        || path.contains(QLatin1Char('\n')))
        return finish(QStringLiteral("discovery_denied"));
    if (m_session->active)
        return finish(QStringLiteral("discovery_busy"));
    const QVariant cursorValue = options.value(QStringLiteral("cursor"));
    const QString cursor = cursorValue.toString();
    if (cursorValue.isValid() && !cursorValue.isNull() && cursorValue.metaType().id() != QMetaType::QString)
        return finish(QStringLiteral("invalid_cursor"));
    if (!cursor.isEmpty()) {
        if (cursor != m_session->cursor || port.toInt() != m_session->port || path != m_session->path)
            return finish(QStringLiteral("invalid_cursor"));
    } else {
        m_session->snapshot = m_session->targets ? m_session->targets() : localLanProbeTargets();
        m_session->next = 0;
        m_session->port = port.toInt();
        m_session->path = path;
    }
    m_session->cursor.clear(); // Consume before I/O: concurrent/replayed pages cannot reuse it.
    m_session->active = this;
    m_end = std::min(m_session->next + limit.toInt(), m_session->snapshot.size());
    m_elapsed.start();
    m_deadline.start(600);
    pump();
}

void LanProbe::pump()
{
    if (m_elapsed.elapsed() >= 600)
        return finish();
    while (!m_finished && m_replies.size() < 4 && m_session->next < m_end) {
        QUrl url;
        url.setScheme(QStringLiteral("http"));
        url.setHost(m_session->snapshot.at(m_session->next++).toString());
        url.setPort(m_session->port);
        const QString origin = url.toString();
        url.setPath(m_session->path, QUrl::StrictMode);
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
        request.setTransferTimeout(600);
        QNetworkReply *reply = m_network.get(request);
        reply->setReadBufferSize(4097);
        m_replies.insert(reply);
        auto body = std::make_shared<QByteArray>();
        const auto take = [reply, body] {
            if (body->size() > 4096 || !reply->isOpen())
                return false;
            body->append(reply->read(4097 - body->size()));
            return body->size() <= 4096;
        };
        connect(reply, &QIODevice::readyRead, this, [reply, take] {
            if (!take())
                reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, body, take, origin] {
            m_replies.remove(reply);
            const bool bounded = take();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (m_elapsed.elapsed() < 600 && bounded && status
                && (reply->error() == QNetworkReply::NoError || status >= 400))
                m_responses.append(QVariantMap { { QStringLiteral("origin"), origin },
                    { QStringLiteral("status"), status }, { QStringLiteral("body"), QString::fromUtf8(*body) } });
            reply->deleteLater();
            pump();
        });
    }
    if (!m_finished && m_replies.isEmpty() && m_session->next >= m_end)
        finish();
}

void LanProbe::abortReplies()
{
    for (auto *reply : std::exchange(m_replies, {})) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

void LanProbe::cancel()
{
    finish(QStringLiteral("action_cancelled"));
}

void LanProbe::finish(QString error)
{
    if (m_finished)
        return;
    m_finished = true;
    m_deadline.stop();
    abortReplies();
    QVariantMap result;
    if (m_session->active == this) {
        m_session->active.clear();
        const bool exhausted = m_session->next >= m_session->snapshot.size();
        m_session->cursor
            = error.isEmpty() && !exhausted ? QUuid::createUuid().toString(QUuid::WithoutBraces) : QString();
        result = { { QStringLiteral("responses"), m_responses }, { QStringLiteral("exhausted"), exhausted },
            { QStringLiteral("cursor"), m_session->cursor.isEmpty() ? QVariant() : QVariant(m_session->cursor) } };
    }
    deleteLater();
    m_completion(std::move(error), std::move(result));
}

} // namespace Spool
