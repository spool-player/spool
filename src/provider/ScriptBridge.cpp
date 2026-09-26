#include "ScriptBridge.h"
#include "SpeedTest.h"

#include <QJSValueIterator>
#include <QNetworkAccessManager>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUdpSocket>
#include <QWebSocket>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Spool {

namespace {
    constexpr qsizetype kMaxResponseBytes = 8 * 1024 * 1024;
    constexpr qsizetype kMaxRequestBytes = 1024 * 1024;
    constexpr int kMaxConcurrentRequests = 4;
    constexpr int kMaxTimers = 16;
    constexpr int kMaxSockets = 4;
    constexpr int kMaxDiscoveryReplies = 64;

    void rejectWith(QJSEngine *engine, QJSValue& reject, const char *code)
    {
        reject.call({ engine->toScriptValue(QString::fromLatin1(code)) });
    }

    int defaultPort(const QUrl& url)
    {
        const QString scheme = url.scheme();
        return url.port(scheme == QStringLiteral("https") || scheme == QStringLiteral("wss") ? 443 : 80);
    }

    bool secure(const QString& scheme)
    {
        return scheme == QStringLiteral("https") || scheme == QStringLiteral("wss");
    }
} // namespace

ScriptWatchdog::ScriptWatchdog(QJSEngine *engine)
    : m_engine(engine)
    , m_thread([this] { run(); })
{
}

ScriptWatchdog::~ScriptWatchdog()
{
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
        m_engine->setInterrupted(true);
    }
    m_changed.notify_one();
    m_thread.join();
}

void ScriptWatchdog::arm()
{
    std::lock_guard lock(m_mutex);
    m_armed = true;
    m_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    m_changed.notify_one();
}

void ScriptWatchdog::disarm()
{
    std::lock_guard lock(m_mutex);
    m_armed = false;
    m_changed.notify_one();
}

void ScriptWatchdog::run()
{
    std::unique_lock lock(m_mutex);
    while (!m_stopping) {
        m_changed.wait(lock, [this] { return m_stopping || m_armed; });
        if (m_stopping)
            break;
        const auto deadline = m_deadline;
        if (!m_changed.wait_until(
                lock, deadline, [this, deadline] { return m_stopping || !m_armed || m_deadline != deadline; })) {
            m_engine->setInterrupted(true);
            m_armed = false;
        }
    }
}

namespace {
    QVariant own(
        const QJSValue& value, int& nodes, qsizetype& bytes, int maximumNodes, qsizetype maximumBytes, int depth)
    {
        if (++nodes > maximumNodes || depth > 20 || bytes > maximumBytes)
            throw std::runtime_error("result_limit");
        if (value.isNull() || value.isUndefined())
            return {};
        if (value.isBool())
            return value.toBool();
        if (value.isNumber()) {
            const double number = value.toNumber();
            if (!std::isfinite(number) || std::abs(number) > 9007199254740991.0)
                throw std::runtime_error("unsafe_number");
            return number;
        }
        if (value.isString()) {
            const QString text = value.toString();
            if ((bytes += text.size() * sizeof(QChar)) > maximumBytes)
                throw std::runtime_error("result_limit");
            return text;
        }
        if (value.isCallable() || value.isQObject() || !value.isObject())
            throw std::runtime_error("invalid_result");
        if (value.isArray()) {
            const quint32 length = value.property(QStringLiteral("length")).toUInt();
            if (length > 10000)
                throw std::runtime_error("result_limit");
            QVariantList list;
            list.reserve(length);
            for (quint32 i = 0; i < length; ++i)
                list.append(own(value.property(i), nodes, bytes, maximumNodes, maximumBytes, depth + 1));
            return list;
        }
        QVariantMap map;
        QJSValueIterator iterator(value);
        while (iterator.hasNext()) {
            iterator.next();
            bytes += iterator.name().size() * sizeof(QChar);
            map.insert(iterator.name(), own(iterator.value(), nodes, bytes, maximumNodes, maximumBytes, depth + 1));
        }
        return map;
    }
} // namespace

QVariant ownScriptValue(const QJSValue& value, int maximumNodes, qsizetype maximumBytes)
{
    int nodes = 0;
    qsizetype bytes = 0;
    return own(value, nodes, bytes, maximumNodes, maximumBytes, 0);
}

bool ScriptAccess::allows(const QUrl& url) const
{
    if (!url.isValid() || !url.userInfo().isEmpty() || url.host().isEmpty())
        return false;
    const QString scheme = url.scheme();
    if (scheme != QStringLiteral("https") && scheme != QStringLiteral("http") && scheme != QStringLiteral("wss")
        && scheme != QStringLiteral("ws"))
        return false;
    return std::any_of(origins.begin(), origins.end(), [&url](const QUrl& origin) {
        return origin.toString() == QStringLiteral("*")
            || (secure(origin.scheme()) == secure(url.scheme()) && origin.host() == url.host()
                && defaultPort(origin) == defaultPort(url));
    });
}

ScriptRequests::ScriptRequests(ScriptAccess *access, QObject *parent)
    : QObject(parent)
    , m_access(access)
{
}

ScriptRequests::~ScriptRequests()
{
    release();
}

void ScriptRequests::release()
{
    m_speedTest = nullptr;
    for (QNetworkReply *reply : std::exchange(m_replies, {})) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    qDeleteAll(std::exchange(m_pending, {}));
}

void ScriptRequests::http(const QString& address, const QVariantMap& options, QJSValue resolve, QJSValue reject)
{
    QJSEngine *engine = m_access->engine;
    const QUrl url(address);
    if (url.scheme().startsWith(QStringLiteral("ws")) || !m_access->allows(url) || m_speedTest
        || m_replies.size() >= kMaxConcurrentRequests)
        return rejectWith(engine, reject, "request_denied");
    const QByteArray method = options.value(QStringLiteral("method"), QStringLiteral("GET")).toString().toLatin1();
    if (method != "GET" && method != "POST" && method != "DELETE" && method != "PUT" && method != "PATCH"
        && method != "HEAD")
        return rejectWith(engine, reject, "method_denied");
    const QByteArray body = options.value(QStringLiteral("body")).toString().toUtf8();
    if (body.size() > kMaxRequestBytes)
        return rejectWith(engine, reject, "request_limit");
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setTransferTimeout(10000);
    const QVariantMap headers = options.value(QStringLiteral("headers")).toMap();
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        const QByteArray name = it.key().toLatin1();
        const QByteArray value = it.value().toString().toUtf8();
        if (name.contains('\r') || name.contains('\n') || value.contains('\r') || value.contains('\n')
            || name.compare("host", Qt::CaseInsensitive) == 0
            || name.compare("content-length", Qt::CaseInsensitive) == 0)
            return rejectWith(engine, reject, "header_denied");
        request.setRawHeader(name, value);
    }
    QNetworkReply *reply = m_access->network->sendCustomRequest(request, method, body);
    reply->setReadBufferSize(256 * 1024);
    m_replies.insert(reply);
    auto buffer = std::make_shared<QByteArray>();
    const auto take = [this, reply, buffer]() {
        const QByteArray chunk = reply->readAll();
        if (buffer->size() + chunk.size() > kMaxResponseBytes)
            return false;
        buffer->append(chunk);
        return true;
    };
    connect(reply, &QIODevice::readyRead, this, [this, take] {
        if (!take())
            emit overflowed();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, take, buffer, resolve, reject]() mutable {
        m_replies.remove(reply);
        reply->deleteLater();
        if (!take()) {
            emit overflowed();
            return;
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QJSEngine *engine = m_access->engine;
        if (status == 0 || (status < 400 && reply->error() != QNetworkReply::NoError))
            return rejectWith(engine, reject, "network_error");
        QJSValue response = engine->newObject();
        response.setProperty(QStringLiteral("status"), status);
        response.setProperty(QStringLiteral("body"), QString::fromUtf8(*buffer));
        const QByteArray location = reply->rawHeader("Location");
        if (!location.isEmpty())
            response.setProperty(QStringLiteral("location"), QString::fromUtf8(location));
        resolve.call({ response });
    });
}

void ScriptRequests::speedTest(const QVariantMap& options, QJSValue resolve, QJSValue reject)
{
    if (m_speedTest || !m_replies.isEmpty() || m_pending.size() >= kMaxTimers)
        return rejectWith(m_access->engine, reject, "request_denied");
    auto *test = new SpeedTest(
        m_access,
        [this, resolve, reject](QString error, qint64 bitrate, int parallelRequests) mutable {
            SpeedTest *test = std::exchange(m_speedTest, nullptr);
            m_pending.remove(test);
            test->deleteLater();
            QJSEngine *engine = m_access->engine;
            if (!error.isEmpty()) {
                reject.call({ engine->toScriptValue(error) });
                return;
            }
            QJSValue result = engine->newObject();
            result.setProperty(QStringLiteral("bitrate"), static_cast<double>(bitrate));
            result.setProperty(QStringLiteral("parallelRequests"), parallelRequests);
            resolve.call({ result });
        },
        this);
    m_speedTest = test;
    m_pending.insert(test);
    test->start(options);
}

void ScriptRequests::delay(int milliseconds, QJSValue resolve, QJSValue reject)
{
    if (milliseconds < 0 || milliseconds > 60000 || m_pending.size() >= kMaxTimers)
        return rejectWith(m_access->engine, reject, "timer_limit");
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    m_pending.insert(timer);
    connect(timer, &QTimer::timeout, this, [this, timer, resolve]() mutable {
        m_pending.remove(timer);
        timer->deleteLater();
        resolve.call();
    });
    timer->start(milliseconds);
}

void ScriptRequests::discover(int port, const QString& message, int timeoutMs, QJSValue resolve, QJSValue reject)
{
    if (port < 1 || port > 65535 || message.size() > 1024 || timeoutMs < 100 || timeoutMs > 5000
        || m_pending.size() >= kMaxTimers)
        return rejectWith(m_access->engine, reject, "discovery_denied");
    auto *socket = new QUdpSocket(this);
    if (!socket->bind(QHostAddress::AnyIPv4, 0)) {
        socket->deleteLater();
        return rejectWith(m_access->engine, reject, "discovery_unavailable");
    }
    m_pending.insert(socket);
    // Every interface's own broadcast address, since the limited broadcast
    // does not leave the default route on some platforms.
    QSet<QHostAddress> targets { QHostAddress::Broadcast };
    for (const QNetworkInterface& interface : QNetworkInterface::allInterfaces()) {
        if (!(interface.flags() & QNetworkInterface::IsUp) || (interface.flags() & QNetworkInterface::IsLoopBack))
            continue;
        for (const QNetworkAddressEntry& entry : interface.addressEntries()) {
            if (!entry.broadcast().isNull())
                targets.insert(entry.broadcast());
        }
    }
    const QByteArray payload = message.toUtf8();
    for (const QHostAddress& target : std::as_const(targets))
        socket->writeDatagram(payload, target, static_cast<quint16>(port));
    auto replies = std::make_shared<QVariantList>();
    connect(socket, &QUdpSocket::readyRead, socket, [socket, replies] {
        while (socket->hasPendingDatagrams()) {
            const QNetworkDatagram datagram = socket->receiveDatagram(8192);
            if (replies->size() < kMaxDiscoveryReplies) {
                replies->append(QVariantMap { { QStringLiteral("address"), datagram.senderAddress().toString() },
                    { QStringLiteral("text"), QString::fromUtf8(datagram.data()) } });
            }
        }
    });
    QTimer::singleShot(timeoutMs, socket, [this, socket, replies, resolve]() mutable {
        m_pending.remove(socket);
        socket->deleteLater();
        resolve.call({ m_access->engine->toScriptValue(*replies) });
    });
}

ScriptOperation::ScriptOperation(
    ScriptAccess *access, std::shared_ptr<ScriptResultSink> sink, QString scope, QObject *parent)
    : QObject(parent)
    , m_access(access)
    , m_sink(std::move(sink))
    , m_scope(std::move(scope))
    , m_requests(access, this)
{
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] { cancel("operation_timeout"); });
    connect(&m_requests, &ScriptRequests::overflowed, this, [this] { cancel("response_limit"); });
    m_deadline.start(15000);
}

ScriptOperation::~ScriptOperation()
{
    if (!m_settled)
        m_sink->reject("runtime_shutdown");
}

void ScriptOperation::cancel(const char *code)
{
    if (m_settled)
        return;
    m_settled = true;
    m_sink->reject(code);
    release();
}

void ScriptOperation::resolve(const QJSValue& result)
{
    if (m_settled)
        return;
    try {
        if (!result.isObject() || result.isArray() || result.isError())
            throw std::runtime_error("invalid_result");
        m_sink->prepare(result);
        if (m_access->engine->isInterrupted())
            throw std::runtime_error("script_interrupted");
        m_settled = true;
        m_sink->complete();
        release();
    } catch (const std::exception&) {
        cancel("invalid_result");
    }
}

void ScriptOperation::reject(const QJSValue& error)
{
    // Provider exceptions can carry tokens and response bodies. Only a short
    // code the provider chose on purpose (an Error whose message looks like
    // one) crosses to native code; anything else becomes provider_error.
    const QString message = error.isError() ? error.property(QStringLiteral("message")).toString() : error.toString();
    static const QRegularExpression code(QStringLiteral("^[a-z][a-z0-9_]{0,47}$"));
    const QByteArray stable = code.match(message).hasMatch() ? message.toLatin1() : QByteArrayLiteral("provider_error");
    if (m_settled)
        return;
    m_settled = true;
    m_sink->reject(stable.constData());
    release();
}

void ScriptOperation::http(const QString& url, const QVariantMap& options, QJSValue resolve, QJSValue reject)
{
    if (!m_settled)
        m_requests.http(url, options, std::move(resolve), std::move(reject));
}

void ScriptOperation::speedTest(const QVariantMap& options, QJSValue resolve, QJSValue reject)
{
    if (!m_settled)
        m_requests.speedTest(options, std::move(resolve), std::move(reject));
}

void ScriptOperation::delay(int milliseconds, QJSValue resolve, QJSValue reject)
{
    if (!m_settled)
        m_requests.delay(std::min(milliseconds, 10000), std::move(resolve), std::move(reject));
}

void ScriptOperation::discover(int port, const QString& message, int timeoutMs, QJSValue resolve, QJSValue reject)
{
    if (!m_settled)
        m_requests.discover(port, message, timeoutMs, std::move(resolve), std::move(reject));
}

void ScriptOperation::release()
{
    m_deadline.stop();
    m_requests.release();
    deleteLater();
}

ScriptSourceHost::ScriptSourceHost(ScriptAccess access, EventSink events, QObject *parent)
    : QObject(parent)
    , m_access(std::move(access))
    , m_events(std::move(events))
    , m_requests(&m_access, this)
{
}

ScriptSourceHost::~ScriptSourceHost()
{
    for (const auto& socket : std::as_const(m_sockets)) {
        if (socket) {
            disconnect(socket, nullptr, this, nullptr);
            socket->abort();
            delete socket;
        }
    }
}

void ScriptSourceHost::emitEvent(const QString& type, const QJSValue& payload)
{
    if (type.isEmpty() || type.size() > 64)
        return;
    try {
        m_events(type, payload.isUndefined() ? QVariantMap {} : ownScriptValue(payload, 5000, 256 * 1024).toMap());
    } catch (const std::exception&) {
        qWarning("provider: dropped an oversized or invalid %s event", qPrintable(type));
    }
}

void ScriptSourceHost::http(const QString& url, const QVariantMap& options, QJSValue resolve, QJSValue reject)
{
    m_requests.http(url, options, std::move(resolve), std::move(reject));
}

void ScriptSourceHost::delay(int milliseconds, QJSValue resolve, QJSValue reject)
{
    m_requests.delay(milliseconds, std::move(resolve), std::move(reject));
}

int ScriptSourceHost::socket(
    const QString& address, const QVariantMap& headers, QJSValue onOpen, QJSValue onMessage, QJSValue onClose)
{
    const QUrl url(address);
    for (auto it = m_sockets.begin(); it != m_sockets.end();)
        it = it.value().isNull() ? m_sockets.erase(it) : std::next(it);
    if (!url.scheme().startsWith(QStringLiteral("ws")) || !m_access.allows(url) || m_sockets.size() >= kMaxSockets)
        return 0;
    QNetworkRequest request(url);
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        const QByteArray value = it.value().toString().toUtf8();
        if (value.contains('\r') || value.contains('\n'))
            return 0;
        request.setRawHeader(it.key().toLatin1(), value);
    }
    auto *socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    socket->setMaxAllowedIncomingMessageSize(4 * 1024 * 1024);
    if (m_access.socketHook)
        m_access.socketHook(socket, url);
    const int id = m_nextSocket++;
    m_sockets.insert(id, socket);
    connect(socket, &QWebSocket::connected, this, [onOpen]() mutable { onOpen.call(); });
    connect(socket, &QWebSocket::textMessageReceived, this,
        [this, onMessage](const QString& text) mutable { onMessage.call({ m_access.engine->toScriptValue(text) }); });
    connect(socket, &QWebSocket::disconnected, this, [this, id, socket, onClose]() mutable {
        m_sockets.remove(id);
        socket->deleteLater();
        onClose.call({ static_cast<int>(socket->closeCode()) });
    });
    socket->open(request);
    return id;
}

void ScriptSourceHost::socketSend(int id, const QString& text)
{
    if (QWebSocket *socket = m_sockets.value(id); socket && text.size() <= 1024 * 1024)
        socket->sendTextMessage(text);
}

void ScriptSourceHost::socketClose(int id)
{
    if (QWebSocket *socket = m_sockets.value(id))
        socket->close();
}

} // namespace Spool
