#include "ScriptBridge.h"
#include "LanProbe.h"
#include "ProviderLogging.h"
#include "SpeedTest.h"
#include "media/MediaTypes.h"

#include <QCryptographicHash>
#include <QJSValueIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
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
#include <limits>
#include <optional>
#include <stdexcept>

#ifdef Q_OS_WIN
#include <windows.h>
#endif
namespace Spool {

Q_LOGGING_CATEGORY(providerMessages, "spool.provider", QtInfoMsg)
Q_LOGGING_CATEGORY(providerTrace, "spool.provider.trace", QtWarningMsg)

bool providerLogEnabled(ProviderLogLevel level)
{
    switch (level) {
    case ProviderLogLevel::Trace:
        return providerTrace().isDebugEnabled();
    case ProviderLogLevel::Debug:
        return providerMessages().isDebugEnabled();
    case ProviderLogLevel::Info:
        return providerMessages().isInfoEnabled();
    case ProviderLogLevel::Warn:
        return providerMessages().isWarningEnabled();
    case ProviderLogLevel::Error:
        return providerMessages().isCriticalEnabled();
    }
    return false;
}

void writeProviderLog(ProviderLogLevel level, const QString& message)
{
    if (!providerLogEnabled(level))
        return;
    // This boundary also protects native provider diagnostics. Never honor
    // --unredacted-urls here, and never allow a provider to inject log lines.
    QString safe = sanitizedLogMessage(message, false).left(4096);
    for (QChar& character : safe) {
        if (character.unicode() < 0x20 || character == QChar(0x7f) || character == QChar(0x2028)
            || character == QChar(0x2029))
            character = QLatin1Char(' ');
    }
    switch (level) {
    case ProviderLogLevel::Trace:
        qCDebug(providerTrace).noquote() << "trace:" << safe;
        break;
    case ProviderLogLevel::Debug:
        qCDebug(providerMessages).noquote() << safe;
        break;
    case ProviderLogLevel::Info:
        qCInfo(providerMessages).noquote() << safe;
        break;
    case ProviderLogLevel::Warn:
        qCWarning(providerMessages).noquote() << safe;
        break;
    case ProviderLogLevel::Error:
        qCCritical(providerMessages).noquote() << safe;
        break;
    }
}
namespace {
    constexpr qsizetype kMaxResponseBytes = 8 * 1024 * 1024;
    constexpr qsizetype kMaxRequestBytes = 1024 * 1024;
    constexpr int kMaxConcurrentRequests = 4;
    constexpr int kMaxTimers = 16;
    constexpr int kMaxSockets = 4;
    constexpr int kMaxDiscoveryReplies = 64;

    std::optional<ProviderLogLevel> logLevel(const QString& level)
    {
        if (level == QLatin1String("trace"))
            return ProviderLogLevel::Trace;
        if (level == QLatin1String("debug"))
            return ProviderLogLevel::Debug;
        if (level == QLatin1String("info"))
            return ProviderLogLevel::Info;
        if (level == QLatin1String("warn"))
            return ProviderLogLevel::Warn;
        if (level == QLatin1String("error"))
            return ProviderLogLevel::Error;
        return {};
    }

    bool credentialField(const QString& key)
    {
        const QString normalized = key.toLower().remove(QLatin1Char('_')).remove(QLatin1Char('-'));
        return normalized.contains(QLatin1String("token")) || normalized.contains(QLatin1String("secret"))
            || normalized.contains(QLatin1String("password")) || normalized.contains(QLatin1String("apikey"))
            || normalized.contains(QLatin1String("authorization")) || normalized.contains(QLatin1String("cookie"))
            || normalized == QLatin1String("pw") || normalized == QLatin1String("code")
            || normalized == QLatin1String("pin");
    }

    bool privateField(const QString& key)
    {
        const QString normalized = key.toLower().remove(QLatin1Char('_')).remove(QLatin1Char('-'));
        return normalized.contains(QLatin1String("name")) || normalized.contains(QLatin1String("title"))
            || normalized.contains(QLatin1String("url")) || normalized.contains(QLatin1String("address"))
            || normalized.contains(QLatin1String("path")) || normalized.contains(QLatin1String("header"))
            || normalized.contains(QLatin1String("body")) || normalized.contains(QLatin1String("response"))
            || normalized.contains(QLatin1String("hash")) || normalized.endsWith(QLatin1String("id"));
    }

    void collectLogSecrets(const QVariant& value, QStringList& secrets, bool sensitive = false)
    {
        if (value.metaType().id() == QMetaType::QVariantMap) {
            const auto map = value.toMap();
            for (auto it = map.cbegin(); it != map.cend(); ++it)
                collectLogSecrets(it.value(), secrets, sensitive || credentialField(it.key()));
        } else if (value.metaType().id() == QMetaType::QVariantList) {
            for (const auto& child : value.toList())
                collectLogSecrets(child, secrets, sensitive);
        } else if (sensitive && value.metaType().id() == QMetaType::QString && !value.toString().isEmpty()) {
            secrets.append(value.toString());
        }
    }

#ifdef Q_OS_WIN
    void *openWorkerThread()
    {
        HANDLE handle = OpenThread(THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
        if (!handle)
            throw std::runtime_error("thread_cpu_unavailable");
        return handle;
    }

    quint64 workerCpuTime(void *handle)
    {
        FILETIME created {}, exited {}, kernel {}, user {};
        if (!GetThreadTimes(handle, &created, &exited, &kernel, &user))
            return std::numeric_limits<quint64>::max();
        const auto ticks
            = [](const FILETIME& value) { return (quint64(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
        return ticks(kernel) + ticks(user);
    }
#endif

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
#ifdef Q_OS_WIN
    , m_workerHandle(openWorkerThread())
#endif
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
#ifdef Q_OS_WIN
    CloseHandle(m_workerHandle);
#endif
}

void ScriptWatchdog::arm()
{
    std::lock_guard lock(m_mutex);
    m_armed = true;
    m_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
#ifdef Q_OS_WIN
    m_cpuStarted = workerCpuTime(m_workerHandle);
    if (m_cpuStarted == std::numeric_limits<quint64>::max()) {
        m_engine->setInterrupted(true);
        m_armed = false;
    }
#endif
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
#ifdef Q_OS_WIN
            // FILETIME is in 100 ns ticks. Native IO and time when this worker
            // was not scheduled cannot make a short script look runaway.
            const quint64 cpuNow = workerCpuTime(m_workerHandle);
            if (cpuNow != std::numeric_limits<quint64>::max() && cpuNow - m_cpuStarted < 5000000) {
                m_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
                continue;
            }
#endif
            m_engine->setInterrupted(true);
            m_armed = false;
        }
    }
}

namespace {
    QVariant own(const QJSValue& value, int& nodes, qsizetype& bytes, int maximumNodes, qsizetype maximumBytes,
        int depth, bool jsonData)
    {
        if (++nodes > maximumNodes || depth > 20 || bytes > maximumBytes)
            throw std::runtime_error("result_limit");
        if (value.isNull())
            return QVariant::fromValue(nullptr);
        if (value.isUndefined())
            return {};
        if (value.isBool())
            return value.toBool();
        if (value.isNumber()) {
            const double number = value.toNumber();
            if (!std::isfinite(number) || (!jsonData && std::abs(number) > 9007199254740991.0))
                throw std::runtime_error("unsafe_number");
            return number;
        }
        if (value.isString()) {
            const QString text = value.toString();
            if ((bytes += text.size() * sizeof(QChar)) > maximumBytes)
                throw std::runtime_error("result_limit");
            return text;
        }
        if (value.isCallable() || value.isQObject() || !value.isObject()
            || (jsonData && (value.isDate() || value.isRegExp() || value.isError())))
            throw std::runtime_error("invalid_result");
        if (value.isArray()) {
            const quint32 length = value.property(QStringLiteral("length")).toUInt();
            if (length > (jsonData ? 32768u : 10000u))
                throw std::runtime_error("result_limit");
            QVariantList list;
            list.reserve(length);
            for (quint32 i = 0; i < length; ++i)
                list.append(own(value.property(i), nodes, bytes, maximumNodes, maximumBytes, depth + 1, jsonData));
            return list;
        }
        QVariantMap map;
        QJSValueIterator iterator(value);
        while (iterator.hasNext()) {
            iterator.next();
            bytes += iterator.name().size() * sizeof(QChar);
            map.insert(
                iterator.name(), own(iterator.value(), nodes, bytes, maximumNodes, maximumBytes, depth + 1, jsonData));
        }
        return map;
    }
} // namespace

QVariant ownScriptValue(const QJSValue& value, int maximumNodes, qsizetype maximumBytes, bool jsonData)
{
    int nodes = 0;
    qsizetype bytes = 0;
    return own(value, nodes, bytes, maximumNodes, maximumBytes, 0, jsonData);
}

bool ScriptAccess::allows(const QUrl& url) const
{
    if (!url.isValid() || !url.userInfo().isEmpty() || url.host().isEmpty())
        return false;
    const QString scheme = url.scheme();
    if (scheme != QStringLiteral("https") && scheme != QStringLiteral("http") && scheme != QStringLiteral("wss")
        && scheme != QStringLiteral("ws"))
        return false;
    const auto matches = [&url](const QUrl& origin) {
        return origin.toString() == QStringLiteral("*")
            || (secure(origin.scheme()) == secure(url.scheme()) && origin.host() == url.host()
                && defaultPort(origin) == defaultPort(url));
    };
    if (std::any_of(origins.cbegin(), origins.cend(), matches))
        return true;
    return std::any_of(stagedOrigins.cbegin(), stagedOrigins.cend(), [&matches](const OriginGrant& grant) {
        return grant.approval->load() && std::any_of(grant.origins.cbegin(), grant.origins.cend(), matches);
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
    QList<QByteArray> requestedHeaders;
    if (options.contains(QStringLiteral("responseHeaders"))) {
        if (!m_access->capabilities.value(QStringLiteral("httpMetadata")).toBool())
            return rejectWith(engine, reject, "unsupported_capability");
        const QVariant value = options.value(QStringLiteral("responseHeaders"));
        if (value.metaType().id() != QMetaType::QVariantList && value.metaType().id() != QMetaType::QStringList)
            return rejectWith(engine, reject, "header_denied");
        const QVariantList names = value.toList();
        if (names.size() > 16)
            return rejectWith(engine, reject, "header_denied");
        static const QRegularExpression token(QStringLiteral("\\A[!#$%&'*+.^_`|~0-9A-Za-z-]{1,256}\\z"));
        for (const QVariant& name : names) {
            if (name.metaType().id() != QMetaType::QString || !token.match(name.toString()).hasMatch())
                return rejectWith(engine, reject, "header_denied");
            const QByteArray normalized = name.toString().toLatin1().toLower();
            if (normalized == "set-cookie" || normalized == "set-cookie2")
                return rejectWith(engine, reject, "header_denied");
            if (!requestedHeaders.contains(normalized))
                requestedHeaders.append(normalized);
        }
    }
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
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, take, buffer, requestedHeaders = std::move(requestedHeaders), resolve, reject]() mutable {
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
            if (!requestedHeaders.isEmpty()) {
                QJSValue headers = engine->newObject();
                qsizetype bytes = 0;
                for (const QByteArray& name : requestedHeaders) {
                    if (!reply->hasRawHeader(name))
                        continue;
                    const QByteArray value = reply->rawHeader(name);
                    bytes += name.size() + value.size();
                    if (bytes > 64 * 1024)
                        return rejectWith(engine, reject, "response_limit");
                    headers.setProperty(QString::fromLatin1(name), QString::fromLatin1(value));
                }
                response.setProperty(QStringLiteral("headers"), headers);
            }
            resolve.call({ response });
        });
}

void ScriptRequests::speedTest(const QVariantMap& options, QJSValue resolve, QJSValue reject)
{
    if (!m_access->capabilities.value(QStringLiteral("speedTest")).toBool())
        return rejectWith(m_access->engine, reject, "unsupported_capability");
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
    if (!m_access->capabilities.value(QStringLiteral("discovery")).toBool())
        return rejectWith(m_access->engine, reject, "unsupported_capability");
    if (port < 1 || port > 65535 || message.size() > 1024 || timeoutMs < 100 || timeoutMs > 5000
        || m_pending.size() >= kMaxTimers)
        return rejectWith(m_access->engine, reject, "discovery_denied");
#if defined(SPOOL_APPLE_MOBILE)
    // UDP broadcast requires Apple's restricted multicast entitlement, which
    // this App Store bundle does not request. Providers can still probe HTTP
    // or accept a manually entered server address after user consent.
    return rejectWith(m_access->engine, reject, "discovery_unavailable");
#endif
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
        QJSValue rows = m_access->engine->newArray(replies->size());
        for (qsizetype index = 0; index < replies->size(); ++index)
            rows.setProperty(index, m_access->engine->toScriptValue(replies->at(index).toMap()));
        resolve.call({ rows });
    });
}

void ScriptRequests::probeLocalHttp(const QVariantMap& options, QJSValue resolve, QJSValue reject)
{
    if (!m_access->capabilities.value(QStringLiteral("lanProbe")).toBool())
        return rejectWith(m_access->engine, reject, "unsupported_capability");
    if (!m_access->loginDraft || !m_access->lanConsent || !m_access->lanSession)
        return rejectWith(m_access->engine, reject, "discovery_denied");
    if (m_pending.size() >= kMaxTimers)
        return rejectWith(m_access->engine, reject, "discovery_busy");
    auto *probe = new LanProbe(
        m_access->lanSession,
        [this, resolve, reject](QString error, QVariantMap result) mutable {
            QJSEngine *engine = m_access->engine;
            if (!error.isEmpty()) {
                reject.call({ engine->toScriptValue(error) });
                return;
            }
            // Qt's QVariantList property wrapper is not a plain JS Array.
            // Publish the promised wire shape even when providers forward it unchanged.
            const QVariantList rows = result.value(QStringLiteral("responses")).toList();
            QJSValue responses = engine->newArray(rows.size());
            for (qsizetype index = 0; index < rows.size(); ++index)
                responses.setProperty(index, engine->toScriptValue(rows.at(index).toMap()));
            QJSValue page = engine->newObject();
            page.setProperty(QStringLiteral("responses"), responses);
            page.setProperty(QStringLiteral("exhausted"), result.value(QStringLiteral("exhausted")).toBool());
            const QVariant cursor = result.value(QStringLiteral("cursor"));
            page.setProperty(QStringLiteral("cursor"),
                cursor.isNull() ? QJSValue(QJSValue::NullValue) : QJSValue(cursor.toString()));
            resolve.call({ page });
        },
        this);
    m_pending.insert(probe);
    connect(probe, &QObject::destroyed, this, [this, probe] { m_pending.remove(probe); });
    // Completion can be synchronous (invalid input or an empty snapshot).
    probe->start(options);
}

ScriptOperation::ScriptOperation(ScriptAccess *access, std::shared_ptr<ScriptResultSink> sink, QString scope,
    QObject *parent, bool activationOperation)
    : QObject(parent)
    , m_access(access)
    , m_sink(std::move(sink))
    , m_scope(std::move(scope))
    , m_requests(access, this)
    , m_activationOperation(activationOperation)
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
    } catch (const std::exception& error) {
        // Only this native decoder contract is exposed; arbitrary exception
        // messages can contain provider data.
        cancel(QByteArrayView(error.what()) == QByteArrayView("invalid_pagination") ? "invalid_pagination"
                                                                                    : "invalid_result");
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
    if (m_access->activationApproval && !m_access->activationApproval->load() && !m_activationOperation) {
        reject.call({ QStringLiteral("account_locked") });
        return;
    }
    if (!m_settled)
        m_requests.http(url, options, std::move(resolve), std::move(reject));
}

void ScriptOperation::speedTest(const QVariantMap& options, QJSValue resolve, QJSValue reject)
{
    if (m_access->activationApproval && !m_access->activationApproval->load() && !m_activationOperation) {
        reject.call({ QStringLiteral("account_locked") });
        return;
    }
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

void ScriptOperation::probeLocalHttp(const QVariantMap& options, QJSValue resolve, QJSValue reject)
{
    if (!m_settled)
        m_requests.probeLocalHttp(options, std::move(resolve), std::move(reject));
}

void ScriptOperation::release()
{
    m_deadline.stop();
    m_requests.release();
    deleteLater();
}

ScriptSourceHost::ScriptSourceHost(ScriptAccess access, EventSink events, QString providerId, QString sourceId,
    const QVariantMap& configuration, QObject *parent)
    : QObject(parent)
    , m_access(std::move(access))
    , m_events(std::move(events))
    , m_requests(&m_access, this)
{
    static const QRegularExpression identifier(QStringLiteral("^[a-z][a-z0-9.-]{0,95}$"));
    if (!identifier.match(providerId).hasMatch())
        providerId = QStringLiteral("unknown");
    const QByteArray account = QCryptographicHash::hash(sourceId.toUtf8(), QCryptographicHash::Sha256).toHex().left(12);
    m_logContext = QStringLiteral("provider=%1 account=%2").arg(providerId, QString::fromLatin1(account));
    collectLogSecrets(configuration, m_logSecrets);
    // Replace longer credentials first when one token is another's prefix.
    std::sort(m_logSecrets.begin(), m_logSecrets.end(),
        [](const QString& left, const QString& right) { return left.size() > right.size(); });
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

bool ScriptSourceHost::isLogEnabled(const QString& level) const
{
    const auto parsed = logLevel(level);
    return parsed && providerLogEnabled(*parsed);
}

void ScriptSourceHost::log(const QString& level, const QJSValue& message, const QJSValue& fields)
{
    const auto parsed = logLevel(level);
    // QJSValue arguments are not converted/serialized before this guard.
    if (!parsed || !providerLogEnabled(*parsed) || !message.isString())
        return;
    const auto redact = [this](QString text) {
        for (const auto& secret : m_logSecrets)
            text.replace(secret, QStringLiteral("<redacted:credential>"));
        return sanitizedLogMessage(std::move(text), false);
    };
    const QString text = message.toString();
    QString safe = text.size() <= 2048 ? redact(text) : QStringLiteral("<dropped:message-limit>");
    QJsonObject metadata;
    if (fields.isObject() && !fields.isArray() && !fields.isCallable()) {
        QJSValueIterator iterator(fields);
        int count = 0;
        qsizetype bytes = 0;
        static const QRegularExpression keyPattern(QStringLiteral("^[a-zA-Z][a-zA-Z0-9_]{0,47}$"));
        while (iterator.hasNext() && count++ < 16) {
            iterator.next();
            const QString key = iterator.name();
            if (!keyPattern.match(key).hasMatch())
                continue;
            QJsonValue value;
            if (credentialField(key))
                value = QStringLiteral("<redacted:credential>");
            else if (privateField(key))
                value = QStringLiteral("<redacted:personal>");
            else {
                const QJSValue child = iterator.value();
                if (child.isString()) {
                    const QString string = child.toString();
                    value = string.size() <= 256 ? redact(string) : QStringLiteral("<dropped:field-limit>");
                } else if (child.isBool())
                    value = child.toBool();
                else if (child.isNull())
                    value = QJsonValue(QJsonValue::Null);
                else if (child.isNumber() && std::isfinite(child.toNumber()))
                    value = child.toNumber();
                else
                    continue;
            }
            bytes += key.size() + (value.isString() ? value.toString().size() : 24);
            if (bytes > 1536)
                break;
            metadata.insert(key, value);
        }
    }
    if (!metadata.isEmpty())
        safe += QLatin1Char(' ') + QString::fromUtf8(QJsonDocument(metadata).toJson(QJsonDocument::Compact));
    writeProviderLog(*parsed, m_logContext + QLatin1Char(' ') + safe);
}

void ScriptSourceHost::emitEvent(const QString& type, const QJSValue& payload)
{
    if (type.isEmpty() || type.size() > 64)
        return;
    try {
        m_events(type, payload.isUndefined() ? QVariantMap {} : ownScriptValue(payload, 5000, 256 * 1024).toMap());
    } catch (const std::exception&) {
        // Capability availability must fail closed even when decoding the event fails.
        if (type == QStringLiteral("capabilitiesChanged"))
            m_events(type, {});
        qWarning("provider: dropped an oversized or invalid %s event", qPrintable(type));
    }
}

void ScriptSourceHost::http(const QString& url, const QVariantMap& options, QJSValue resolve, QJSValue reject)
{
    if (m_access.activationApproval && !m_access.activationApproval->load()) {
        reject.call({ QStringLiteral("account_locked") });
        return;
    }
    m_requests.http(url, options, std::move(resolve), std::move(reject));
}

void ScriptSourceHost::delay(int milliseconds, QJSValue resolve, QJSValue reject)
{
    m_requests.delay(milliseconds, std::move(resolve), std::move(reject));
}

int ScriptSourceHost::socket(
    const QString& address, const QVariantMap& headers, QJSValue onOpen, QJSValue onMessage, QJSValue onClose)
{
    if (m_access.activationApproval && !m_access.activationApproval->load())
        return 0;
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
