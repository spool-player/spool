#include "ScriptRuntime.h"

#include <QAbstractEventDispatcher>
#include <QCoroFuture>
#include <QElapsedTimer>
#include <QFuture>
#include <QJSEngine>
#include <QJSValueIterator>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPromise>
#include <QSet>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace JellyfinNative {
namespace {
    using Completion = std::shared_ptr<QPromise<QVariantMap>>;

    void fail(const Completion& completion, const char *code)
    {
        completion->setException(std::make_exception_ptr(std::runtime_error(code)));
        completion->finish();
    }

    QCoro::Task<QVariantMap> awaitResult(QFuture<QVariantMap> future)
    {
        auto result = qCoro(future);
        co_return co_await result.takeResult();
    }

    // Sleeps while JS is idle. Qt explicitly permits setInterrupted() from another
    // thread. The mutex protects engine lifetime, not JS heap access.
    class ScriptWatchdog {
    public:
        explicit ScriptWatchdog(QJSEngine *engine)
            : m_engine(engine)
            , m_thread([this] { run(); })
        {
        }
        ~ScriptWatchdog()
        {
            {
                std::lock_guard lock(m_mutex);
                m_stopping = true;
                m_engine->setInterrupted(true);
            }
            m_changed.notify_one();
            m_thread.join();
        }
        void arm()
        {
            std::lock_guard lock(m_mutex);
            m_armed = true;
            m_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            m_changed.notify_one();
        }
        void disarm()
        {
            std::lock_guard lock(m_mutex);
            m_armed = false;
            m_changed.notify_one();
        }
        void interrupt()
        {
            std::lock_guard lock(m_mutex);
            m_engine->setInterrupted(true);
        }

    private:
        void run()
        {
            std::unique_lock lock(m_mutex);
            while (!m_stopping) {
                m_changed.wait(lock, [this] { return m_stopping || m_armed; });
                if (m_stopping)
                    break;
                const auto deadline = m_deadline;
                if (!m_changed.wait_until(lock, deadline,
                        [this, deadline] { return m_stopping || !m_armed || m_deadline != deadline; })) {
                    m_engine->setInterrupted(true);
                    m_armed = false;
                }
            }
        }
        QJSEngine *m_engine;
        std::mutex m_mutex;
        std::condition_variable m_changed;
        bool m_stopping = false;
        bool m_armed = false;
        std::chrono::steady_clock::time_point m_deadline;
        std::thread m_thread;
    };

    // Validation and ownership conversion in one walk; no JSON stringify/parse
    // round-trip, no JS object retained by native models. Limits also reject cycles.
    QVariant ownValue(const QJSValue& value, int& nodes, qsizetype& bytes, int depth = 0)
    {
        if (++nodes > 50000 || depth > 20 || bytes > 4 * 1024 * 1024)
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
            bytes += text.size() * sizeof(QChar);
            if (bytes > 4 * 1024 * 1024)
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
                list.append(ownValue(value.property(i), nodes, bytes, depth + 1));
            return list;
        }
        QVariantMap map;
        QJSValueIterator iterator(value);
        while (iterator.hasNext()) {
            iterator.next();
            bytes += iterator.name().size() * sizeof(QChar);
            map.insert(iterator.name(), ownValue(iterator.value(), nodes, bytes, depth + 1));
        }
        return map;
    }

    bool sameOrigin(const QUrl& left, const QUrl& right)
    {
        return left.scheme() == right.scheme() && left.host() == right.host()
            && left.port(left.scheme() == QStringLiteral("https") ? 443 : 80)
            == right.port(right.scheme() == QStringLiteral("https") ? 443 : 80);
    }
} // namespace

class ScriptOperation final : public QObject {
    Q_OBJECT
public:
    ScriptOperation(QJSEngine *engine, QNetworkAccessManager *network, ScriptWatchdog *watchdog, Completion completion,
        QList<QUrl> origins, QObject *parent)
        : QObject(parent)
        , engine(engine)
        , network(network)
        , watchdog(watchdog)
        , completion(std::move(completion))
        , origins(std::move(origins))
    {
        deadline.setSingleShot(true);
        deadline.setInterval(15000);
        connect(&deadline, &QTimer::timeout, this, [this] { cancel("operation_timeout"); });
        deadline.start();
    }
    ~ScriptOperation() override
    {
        if (!settled)
            fail(completion, "runtime_shutdown");
    }
    void cancel(const char *code)
    {
        if (settled)
            return;
        settled = true;
        fail(completion, code);
        release();
    }
    Q_INVOKABLE void resolve(const QJSValue& result)
    {
        if (settled)
            return;
        try {
            if (!result.isObject() || result.isArray() || result.isError())
                throw std::runtime_error("invalid_result");
            int nodes = 0;
            qsizetype bytes = 0;
            QVariantMap owned = ownValue(result, nodes, bytes).toMap();
            if (engine->isInterrupted())
                throw std::runtime_error("script_interrupted");
            settled = true;
            completion->addResult(std::move(owned));
            completion->finish();
            release();
        } catch (const std::exception&) {
            cancel("invalid_result");
        }
    }
    Q_INVOKABLE void reject(const QJSValue&)
    {
        // Provider exceptions can carry tokens and response bodies. The public
        // error is a stable code, never the untrusted exception's string value.
        cancel("provider_error");
    }
    Q_INVOKABLE void request(
        const QString& address, const QVariantMap& options, QJSValue resolveCallback, QJSValue rejectCallback)
    {
        if (settled)
            return;
        const QUrl url(address);
        const bool allowed = url.isValid() && url.userInfo().isEmpty()
            && (url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("http"))
            && std::any_of(
                origins.begin(), origins.end(), [&url](const QUrl& origin) { return sameOrigin(url, origin); });
        if (!allowed || replies.size() >= 4) {
            rejectCallback.call({ engine->toScriptValue(QStringLiteral("request_denied")) });
            return;
        }
        const QByteArray method = options.value(QStringLiteral("method"), QStringLiteral("GET")).toString().toLatin1();
        if (method != "GET" && method != "POST" && method != "DELETE" && method != "PUT" && method != "PATCH") {
            rejectCallback.call({ engine->toScriptValue(QStringLiteral("method_denied")) });
            return;
        }
        const QByteArray body = options.value(QStringLiteral("body")).toString().toUtf8();
        if (body.size() > 1024 * 1024) {
            rejectCallback.call({ engine->toScriptValue(QStringLiteral("request_limit")) });
            return;
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
                || name.compare("content-length", Qt::CaseInsensitive) == 0) {
                rejectCallback.call({ engine->toScriptValue(QStringLiteral("header_denied")) });
                return;
            }
            request.setRawHeader(name, value);
        }
        QNetworkReply *reply = network->sendCustomRequest(request, method, body);
        reply->setReadBufferSize(256 * 1024);
        replies.insert(reply);
        auto buffer = std::make_shared<QByteArray>();
        connect(reply, &QIODevice::readyRead, this, [this, reply, buffer] {
            const QByteArray chunk = reply->readAll();
            if (buffer->size() + chunk.size() > 8 * 1024 * 1024) {
                cancel("response_limit");
                return;
            }
            buffer->append(chunk);
        });
        connect(
            reply, &QNetworkReply::finished, this, [this, reply, buffer, resolveCallback, rejectCallback]() mutable {
                replies.remove(reply);
                reply->deleteLater();
                if (settled)
                    return;
                const QByteArray finalBytes = reply->readAll();
                if (buffer->size() + finalBytes.size() > 8 * 1024 * 1024) {
                    cancel("response_limit");
                    return;
                }
                buffer->append(finalBytes);
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                watchdog->arm();
                if (status == 0 || (status < 400 && reply->error() != QNetworkReply::NoError)) {
                    rejectCallback.call({ engine->toScriptValue(QStringLiteral("network_error")) });
                } else {
                    QJSValue response = engine->newObject();
                    response.setProperty(QStringLiteral("status"), status);
                    response.setProperty(QStringLiteral("body"), QString::fromUtf8(*buffer));
                    resolveCallback.call({ response });
                }
                if (engine->isInterrupted())
                    cancel("script_interrupted");
            });
    }

private:
    void release()
    {
        deadline.stop();
        const auto pending = std::exchange(replies, {});
        for (QNetworkReply *reply : pending) {
            disconnect(reply, nullptr, this, nullptr);
            reply->abort();
            reply->deleteLater();
        }
        deleteLater();
    }
    QJSEngine *engine;
    QNetworkAccessManager *network;
    ScriptWatchdog *watchdog;
    Completion completion;
    QList<QUrl> origins;
    QTimer deadline;
    QSet<QNetworkReply *> replies;
    bool settled = false;
};

class ScriptWorker final : public QObject {
public:
    explicit ScriptWorker(QString entryPoint)
        : entryPoint(std::move(entryPoint))
    {
    }
    void initialize()
    {
        engine = std::make_unique<QJSEngine>();
        watchdog = std::make_unique<ScriptWatchdog>(engine.get());
        network = std::make_unique<QNetworkAccessManager>();
        auto *dispatcher = QAbstractEventDispatcher::instance();
        connect(dispatcher, &QAbstractEventDispatcher::awake, this, [this] {
            if (watchdog)
                watchdog->arm();
        });
        connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, this, [this] {
            if (!watchdog)
                return;
            watchdog->disarm();
            if (engine->isInterrupted()) {
                for (auto& source : sources)
                    for (ScriptOperation *operation : source.operations)
                        operation->cancel("script_interrupted");
            }
        });
        watchdog->arm();
        module = engine->importModule(entryPoint);
        // Promise syntax is the portable baseline; no Node/browser globals or
        // async-function syntax is required from the bundled Qt JS engine.
        invoke = engine->evaluate(QStringLiteral(R"JS(
            (function(source, method, args, bridge) {
                const host = Object.freeze({
                    http: function(url, options) {
                        return new Promise(function(resolve, reject) {
                            bridge.request(url, options || {}, resolve, reject);
                        });
                    }
                });
                try {
                    Promise.resolve(source[method](args, host)).then(
                        function(value) { bridge.resolve(value); },
                        function(error) { bridge.reject(error); });
                } catch (error) { bridge.reject(error); }
            })
        )JS"));
    }
    void add(QString id, QVariantMap config, QList<QUrl> origins, const Completion& completion)
    {
        if (!engine)
            initialize();
        if (engine->isInterrupted() || module.isError()) {
            fail(completion, "module_unavailable");
            return;
        }
        if (id.isEmpty() || sources.contains(id) || sources.size() >= 16) {
            fail(completion, "source_conflict_or_limit");
            return;
        }
        watchdog->arm();
        QJSValue source = module.property(QStringLiteral("createSource")).call({ engine->toScriptValue(config) });
        if (source.isError() || !source.isObject() || engine->isInterrupted()) {
            fail(completion, "source_initialization_failed");
            return;
        }
        sources.insert(id, { source, std::move(origins), {} });
        completion->addResult(QVariantMap { { QStringLiteral("sourceId"), id } });
        completion->finish();
    }
    void call(const QString& id, const QString& method, const QVariantMap& args, const Completion& completion)
    {
        auto source = sources.find(id);
        if (!engine || engine->isInterrupted() || source == sources.end()) {
            fail(completion, "source_unavailable");
            return;
        }
        if (source->operations.size() >= 8 || activeOperations >= 32) {
            fail(completion, "operation_limit");
            return;
        }
        if (method.startsWith(QLatin1Char('_')) || !source->object.hasOwnProperty(method)
            || !source->object.property(method).isCallable()) {
            fail(completion, "unsupported_operation");
            return;
        }
        auto *operation
            = new ScriptOperation(engine.get(), network.get(), watchdog.get(), completion, source->origins, this);
        source->operations.insert(operation);
        ++activeOperations;
        connect(operation, &QObject::destroyed, this, [this, id, operation] {
            --activeOperations;
            auto source = sources.find(id);
            if (source != sources.end())
                source->operations.remove(operation);
        });
        QJSEngine::setObjectOwnership(operation, QJSEngine::CppOwnership);
        watchdog->arm();
        invoke.call({ source->object, QJSValue(method), engine->toScriptValue(args), engine->newQObject(operation) });
        if (engine->isInterrupted()) {
            for (auto& source : sources) {
                for (ScriptOperation *pending : source.operations)
                    pending->cancel("script_interrupted");
            }
        }
    }
    void remove(const QString& id)
    {
        auto source = sources.find(id);
        if (source == sources.end())
            return;
        for (ScriptOperation *operation : source->operations)
            operation->cancel("source_removed");
        sources.erase(source);
    }
    void shutdown()
    {
        const auto ids = sources.keys();
        for (const auto& id : ids)
            remove(id);
        // Destroy bridges before the engine and its JS callbacks.
        const auto operations = findChildren<ScriptOperation *>(QString(), Qt::FindDirectChildrenOnly);
        qDeleteAll(operations);
        invoke = QJSValue();
        module = QJSValue();
        watchdog.reset();
        network.reset();
        engine.reset();
    }

private:
    struct Source {
        QJSValue object;
        QList<QUrl> origins;
        QSet<ScriptOperation *> operations;
    };
    QString entryPoint;
    std::unique_ptr<QJSEngine> engine;
    std::unique_ptr<ScriptWatchdog> watchdog;
    std::unique_ptr<QNetworkAccessManager> network;
    QJSValue module;
    QJSValue invoke;
    QHash<QString, Source> sources;
    int activeOperations = 0;
};

struct ScriptRuntime::Private {
    QThread thread;
    ScriptWorker *worker;
    std::atomic<int> queued = 0;
};

ScriptRuntime::ScriptRuntime(QString entryPoint, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
    d->worker = new ScriptWorker(std::move(entryPoint));
    d->worker->moveToThread(&d->thread);
    connect(&d->thread, &QThread::finished, d->worker, &QObject::deleteLater);
    d->thread.setObjectName(QStringLiteral("provider-js"));
    d->thread.start();
}

ScriptRuntime::~ScriptRuntime()
{
    QMetaObject::invokeMethod(d->worker, [worker = d->worker] { worker->shutdown(); }, Qt::BlockingQueuedConnection);
    d->thread.quit();
    d->thread.wait();
}

QCoro::Task<QVariantMap> ScriptRuntime::addSource(QString sourceId, QVariantMap configuration, QList<QUrl> origins)
{
    auto completion = std::make_shared<QPromise<QVariantMap>>();
    completion->start();
    auto future = completion->future();
    if (d->queued.fetch_add(1) >= 64) {
        --d->queued;
        fail(completion, "queue_limit");
        return awaitResult(std::move(future));
    }
    QMetaObject::invokeMethod(
        d->worker,
        [state = d.get(), worker = d->worker, sourceId = std::move(sourceId), configuration = std::move(configuration),
            origins = std::move(origins), completion]() mutable {
            --state->queued;
            worker->add(std::move(sourceId), std::move(configuration), std::move(origins), completion);
        },
        Qt::QueuedConnection);
    return awaitResult(std::move(future));
}

QCoro::Task<QVariantMap> ScriptRuntime::call(QString sourceId, QString method, QVariantMap arguments)
{
    auto completion = std::make_shared<QPromise<QVariantMap>>();
    completion->start();
    auto future = completion->future();
    if (d->queued.fetch_add(1) >= 64) {
        --d->queued;
        fail(completion, "queue_limit");
        return awaitResult(std::move(future));
    }
    QMetaObject::invokeMethod(
        d->worker,
        [state = d.get(), worker = d->worker, sourceId = std::move(sourceId), method = std::move(method),
            arguments = std::move(arguments), completion] {
            --state->queued;
            worker->call(sourceId, method, arguments, completion);
        },
        Qt::QueuedConnection);
    return awaitResult(std::move(future));
}

void ScriptRuntime::removeSource(const QString& sourceId)
{
    QMetaObject::invokeMethod(
        d->worker, [worker = d->worker, sourceId] { worker->remove(sourceId); }, Qt::QueuedConnection);
}

} // namespace JellyfinNative

#include "ScriptRuntime.moc"
