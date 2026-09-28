#include "ScriptRuntime.h"

#include "LanProbe.h"
#include "ScriptBridge.h"

#include <QAbstractEventDispatcher>
#include <QCoroFuture>
#include <QFuture>
#include <QNetworkAccessManager>
#include <QPromise>
#include <QThread>

#include <atomic>
#include <optional>
#include <stdexcept>

namespace Spool {
namespace {
    template <typename T> void fail(const std::shared_ptr<QPromise<T>>& promise, const char *code)
    {
        promise->setException(std::make_exception_ptr(std::runtime_error(code)));
        promise->finish();
    }

    template <typename T> QCoro::Task<T> awaitResult(QFuture<T> future)
    {
        auto result = qCoro(future);
        co_return co_await result.takeResult();
    }

    // QVariant container conversion can produce Qt sequence wrappers rather
    // than JavaScript arrays, including inside an otherwise ordinary object.
    // Provider Value trees must have native JS containers all the way down.
    QJSValue scriptValue(
        QJSEngine *engine, const QVariant& value, int& nodes, qsizetype& bytes, int depth, int maximumArray)
    {
        if (++nodes > 50000 || depth > 20 || bytes > 4 * 1024 * 1024)
            throw std::runtime_error("argument_limit");
        const auto countText = [&bytes](const QString& text) {
            bytes += text.size() * sizeof(QChar);
            if (bytes > 4 * 1024 * 1024)
                throw std::runtime_error("argument_limit");
        };
        switch (value.metaType().id()) {
        case QMetaType::UnknownType:
            return QJSValue(QJSValue::UndefinedValue);
        case QMetaType::Nullptr:
            return QJSValue(QJSValue::NullValue);
        case QMetaType::QVariantMap: {
            QJSValue object = engine->newObject();
            // Populate own data properties without invoking inherited setters
            // (notably __proto__), then retain ordinary object compatibility.
            const QJSValue prototype = object.prototype();
            object.setPrototype(QJSValue(QJSValue::NullValue));
            const auto map = value.toMap();
            for (auto it = map.cbegin(); it != map.cend(); ++it) {
                countText(it.key());
                object.setProperty(it.key(), scriptValue(engine, it.value(), nodes, bytes, depth + 1, maximumArray));
            }
            object.setPrototype(prototype);
            return object;
        }
        case QMetaType::QVariantList: {
            const auto list = value.toList();
            if (list.size() > maximumArray)
                throw std::runtime_error("argument_limit");
            QJSValue array = engine->newArray(static_cast<uint>(list.size()));
            const QJSValue prototype = array.prototype();
            array.setPrototype(QJSValue(QJSValue::NullValue));
            for (qsizetype index = 0; index < list.size(); ++index)
                array.setProperty(static_cast<uint>(index),
                    scriptValue(engine, list.at(index), nodes, bytes, depth + 1, maximumArray));
            array.setPrototype(prototype);
            return array;
        }
        case QMetaType::QStringList: {
            const auto list = value.toStringList();
            if (list.size() > maximumArray)
                throw std::runtime_error("argument_limit");
            QJSValue array = engine->newArray(static_cast<uint>(list.size()));
            const QJSValue prototype = array.prototype();
            array.setPrototype(QJSValue(QJSValue::NullValue));
            for (qsizetype index = 0; index < list.size(); ++index)
                array.setProperty(static_cast<uint>(index),
                    scriptValue(engine, list.at(index), nodes, bytes, depth + 1, maximumArray));
            array.setPrototype(prototype);
            return array;
        }
        case QMetaType::QString:
            countText(value.toString());
            break;
        case QMetaType::Bool:
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
        case QMetaType::Double:
        case QMetaType::Float:
        case QMetaType::Long:
        case QMetaType::ULong:
        case QMetaType::Short:
        case QMetaType::UShort:
        case QMetaType::Char:
        case QMetaType::SChar:
        case QMetaType::UChar:
            break;
        default:
            throw std::runtime_error("invalid_arguments");
        }
        return engine->toScriptValue(value);
    }

    QJSValue scriptValue(QJSEngine *engine, const QVariantMap& value, int maximumArray = 10000)
    {
        int nodes = 0;
        qsizetype bytes = 0;
        return scriptValue(engine, value, nodes, bytes, 0, maximumArray);
    }

    template <typename T, typename Decoder> class TypedSink final : public ScriptResultSink {
    public:
        TypedSink(std::shared_ptr<QPromise<T>> promise, Decoder decoder)
            : m_promise(std::move(promise))
            , m_decoder(std::move(decoder))
        {
        }
        void prepare(const QJSValue& value) override
        {
            m_result.emplace(m_decoder(value));
        }
        void complete() override
        {
            m_promise->addResult(std::move(*m_result));
            m_promise->finish();
        }
        void reject(const char *code) override
        {
            fail(m_promise, code);
        }

    private:
        std::shared_ptr<QPromise<T>> m_promise;
        Decoder m_decoder;
        std::optional<T> m_result;
    };

    // Promise syntax is the baseline: no Node/browser globals are provided and
    // none are required. Source-level services outlive any one operation.
    constexpr auto kGlue = R"JS((function() {
        function promised(start) {
            return new Promise(function(resolve, reject) { start(resolve, reject); });
        }
        function sourceHost(bridge, device, extensions) {
            return Object.freeze({
                device: device,
                extensions: extensions,
                http: function(url, options) {
                    return promised(function(ok, no) { bridge.http(String(url), options || {}, ok, no); });
                },
                delay: function(ms) {
                    return promised(function(ok, no) { bridge.delay(ms | 0, ok, no); });
                },
                emit: function(type, payload) { bridge.emitEvent(String(type), payload); },
                socket: function(url, options) {
                    const socket = {onopen: null, onmessage: null, onclose: null};
                    const id = bridge.socket(String(url), (options && options.headers) || {},
                        function() { if (socket.onopen) socket.onopen(); },
                        function(text) { if (socket.onmessage) socket.onmessage(text); },
                        function(code) { if (socket.onclose) socket.onclose(code); });
                    if (!id)
                        throw new Error('socket_denied');
                    socket.send = function(text) { bridge.socketSend(id, String(text)); };
                    socket.close = function() { bridge.socketClose(id); };
                    return socket;
                }
            });
        }
        return {
            create: function(module, configuration, bridge, device, extensions) {
                return module.createSource(configuration, sourceHost(bridge, device, extensions));
            },
            call: function(source, method, args, operation, bridge, device, extensions) {
                const host = Object.freeze({
                    device: device,
                    extensions: extensions,
                    http: function(url, options) {
                        return promised(function(ok, no) { operation.http(String(url), options || {}, ok, no); });
                    },
                    speedTest: function(options) {
                        if (!options || typeof options.url !== 'string')
                            return Promise.reject(new Error('request_denied'));
                        return promised(function(ok, no) { operation.speedTest(options, ok, no); });
                    },
                    delay: function(ms) {
                        if (!Number.isInteger(ms) || ms < 0 || ms > 10000)
                            return Promise.reject(new Error('timer_limit'));
                        return promised(function(ok, no) { operation.delay(ms, ok, no); });
                    },
                    discover: function(options) {
                        options = options || {};
                        return promised(function(ok, no) {
                            operation.discover(options.port | 0, String(options.message || ''),
                                options.timeout === undefined ? 1500 : options.timeout | 0, ok, no);
                        });
                    },
                    probeLocalHttp: function(options) {
                        return promised(function(ok, no) { operation.probeLocalHttp(options || {}, ok, no); });
                    },
                    emit: function(type, payload) { bridge.emitEvent(String(type), payload); }
                });
                try {
                    Promise.resolve(source[method](args, host)).then(
                        function(value) { operation.resolve(value); },
                        function(error) { operation.reject(error); });
                } catch (error) { operation.reject(error); }
            }
        };
    })())JS";
} // namespace

class ScriptWorker final : public QObject {
public:
    using EventSink = std::function<void(const QString&, quint64, const QString&, const QVariantMap&)>;

    ScriptWorker(QString entryPoint, QVariantMap device, ScriptRuntime::NetworkHooks hooks, EventSink events,
        std::function<void()> interrupted)
        : m_entryPoint(std::move(entryPoint))
        , m_device(std::move(device))
        , m_hooks(std::move(hooks))
        , m_events(std::move(events))
        , m_interrupted(std::move(interrupted))
    {
    }

    void add(const QString& id, const QVariantMap& configuration, const QList<QUrl>& origins,
        const QVariantMap& extensions, bool loginDraft, std::shared_ptr<std::atomic_bool> activationApproval,
        quint64 generation, const std::shared_ptr<QPromise<QVariantMap>>& promise)
    {
        if (!m_engine)
            initialize();
        if (m_engine->isInterrupted() || m_module.isError()
            || !m_module.property(QStringLiteral("createSource")).isCallable())
            return fail(promise, "module_unavailable");
        if (id.isEmpty() || m_sources.contains(id) || m_sources.size() >= 32)
            return fail(promise, "source_conflict_or_limit");
        QJSValue jsConfiguration;
        try {
            jsConfiguration = scriptValue(m_engine.get(), configuration);
        } catch (const std::exception& error) {
            return fail(promise, error.what());
        }
        ScriptAccess access { m_engine.get(), m_network.get(), origins, m_hooks.socket };
        access.extensions = extensions;
        access.loginDraft = loginDraft;
        access.activationApproval = std::move(activationApproval);
        if (loginDraft && extensions.value(QStringLiteral("spool.lan-probe")).toInt() == 1) {
            access.lanSession = std::make_shared<LanProbeSession>();
            access.lanSession->targets = m_hooks.lanTargets;
        }
        auto *host = new ScriptSourceHost(
            access,
            [events = m_events, id, generation](
                const QString& type, const QVariantMap& payload) { events(id, generation, type, payload); },
            this);
        QJSEngine::setObjectOwnership(host, QJSEngine::CppOwnership);
        const QJSValue bridge = m_engine->newQObject(host);
        m_watchdog->arm();
        const QJSValue jsExtensions = m_engine->toScriptValue(extensions);
        m_engine->globalObject()
            .property(QStringLiteral("Object"))
            .property(QStringLiteral("freeze"))
            .call({ jsExtensions });
        const QJSValue object = m_glue.property(QStringLiteral("create"))
                                    .call({ m_module, jsConfiguration, bridge, m_jsDevice, jsExtensions });
        if (object.isError() || !object.isObject() || m_engine->isInterrupted()) {
            delete host;
            checkInterrupted();
            return fail(promise, "source_initialization_failed");
        }
        m_sources.insert(id, { object, bridge, jsExtensions, host, {}, generation });
        promise->addResult(QVariantMap { { QStringLiteral("sourceId"), id } });
        promise->finish();
    }

    void grantOrigins(const QString& id, quint64 generation, const QList<QUrl>& origins,
        const std::shared_ptr<std::atomic_bool>& approval, const std::shared_ptr<QPromise<bool>>& promise)
    {
        const auto source = m_sources.find(id);
        if (source == m_sources.end() || source->generation != generation)
            return fail(promise, "source_unavailable");
        auto *access = source->host->access();
        if (access->extensions.value(QStringLiteral("spool.origin-grants")).toInt() != 1)
            return fail(promise, "unsupported_extension");
        QList<QUrl> normalized;
        for (QUrl origin : origins) {
            if (!origin.isValid() || origin.host().isEmpty() || origin.host().contains(QLatin1Char('*'))
                || origin.authority().contains(QLatin1Char('@')) || origin.hasQuery() || origin.hasFragment()
                || (origin.path() != QString() && origin.path() != QStringLiteral("/"))
                || (origin.scheme() != QStringLiteral("http") && origin.scheme() != QStringLiteral("https"))
                || origin.port(80) < 1)
                return fail(promise, "origin_denied");
            origin.setPath({});
            if (origin.port() == (origin.scheme() == QStringLiteral("https") ? 443 : 80))
                origin.setPort(-1);
            if (!access->origins.contains(origin) && !normalized.contains(origin))
                normalized.append(origin);
        }
        for (auto it = access->stagedOrigins.begin(); it != access->stagedOrigins.end();) {
            if (it->approval->load()) {
                for (const auto& origin : std::as_const(it->origins)) {
                    if (!access->origins.contains(origin))
                        access->origins.append(origin);
                }
                it = access->stagedOrigins.erase(it);
            } else if (it->approval.use_count() == 1) {
                it = access->stagedOrigins.erase(it);
            } else {
                ++it;
            }
        }
        if (approval)
            access->stagedOrigins.append({ std::move(normalized), approval });
        else
            access->origins.append(normalized);
        promise->addResult(true);
        promise->finish();
    }

    void allowLanDiscovery(const QString& id, quint64 generation, const std::shared_ptr<QPromise<bool>>& promise)
    {
        const auto source = m_sources.find(id);
        if (source == m_sources.end() || source->generation != generation)
            return fail(promise, "source_unavailable");
        auto *access = source->host->access();
        if (!access->loginDraft || access->extensions.value(QStringLiteral("spool.lan-probe")).toInt() != 1)
            return fail(promise, "unsupported_extension");
        access->lanConsent = true;
        promise->addResult(true);
        promise->finish();
    }

    void cancelLanDiscovery(const QString& id, quint64 generation)
    {
        const auto source = m_sources.find(id);
        if (source == m_sources.end() || source->generation != generation)
            return;
        auto *access = source->host->access();
        access->lanConsent = false;
        auto& session = access->lanSession;
        if (!session)
            return;
        if (session->active)
            session->active->cancel();
        session->cursor.clear();
        session->snapshot.clear();
    }

    void call(const QString& id, const QString& method, const QVariantMap& args, const QString& scope,
        const std::shared_ptr<ScriptResultSink>& sink)
    {
        const auto source = m_sources.find(id);
        if (!m_engine || m_engine->isInterrupted() || source == m_sources.end())
            return sink->reject("source_unavailable");
        const auto approval = source->host->access()->activationApproval;
        if (approval && !approval->load() && method != QStringLiteral("describe")
            && method != QStringLiteral("activate"))
            return sink->reject("account_locked");
        if (source->operations.size() >= 8 || m_activeOperations >= 32)
            return sink->reject("operation_limit");
        if (method.startsWith(QLatin1Char('_')) || !source->object.hasOwnProperty(method)
            || !source->object.property(method).isCallable())
            return sink->reject("unsupported_operation");
        QJSValue jsArguments;
        try {
            jsArguments = scriptValue(m_engine.get(), args, method == QStringLiteral("dataWrite") ? 32768 : 10000);
        } catch (const std::exception& error) {
            return sink->reject(error.what());
        }
        auto *operation
            = new ScriptOperation(source->host->access(), sink, scope, this, method == QStringLiteral("activate"));
        source->operations.insert(operation);
        ++m_activeOperations;
        connect(operation, &QObject::destroyed, this, [this, id, operation] {
            --m_activeOperations;
            if (const auto found = m_sources.find(id); found != m_sources.end())
                found->operations.remove(operation);
        });
        QJSEngine::setObjectOwnership(operation, QJSEngine::CppOwnership);
        m_watchdog->arm();
        m_glue.property(QStringLiteral("call"))
            .call({ source->object, QJSValue(method), jsArguments, m_engine->newQObject(operation), source->bridge,
                m_jsDevice, source->extensions });
        checkInterrupted();
    }

    void cancelScope(const QString& id, const QString& scope)
    {
        const auto source = m_sources.constFind(id);
        if (scope.isEmpty() || source == m_sources.cend())
            return;
        for (ScriptOperation *operation : source->operations) {
            if (operation->scope() == scope)
                operation->cancel("action_cancelled");
        }
    }

    void remove(const QString& id)
    {
        const auto source = m_sources.find(id);
        if (source == m_sources.end())
            return;
        for (ScriptOperation *operation : std::as_const(source->operations))
            operation->cancel("source_removed");
        delete source->host;
        m_sources.erase(source);
    }

    void shutdown()
    {
        for (const QString& id : m_sources.keys())
            remove(id);
        // Bridges hold JS callbacks: destroy them before the engine.
        qDeleteAll(findChildren<ScriptOperation *>(QString(), Qt::FindDirectChildrenOnly));
        m_glue = {};
        m_module = {};
        m_jsDevice = {};
        m_watchdog.reset();
        m_network.reset();
        m_engine.reset();
    }

private:
    struct Source {
        QJSValue object;
        QJSValue bridge;
        QJSValue extensions;
        ScriptSourceHost *host = nullptr;
        QSet<ScriptOperation *> operations;
        quint64 generation = 0;
    };

    void initialize()
    {
        m_engine = std::make_unique<QJSEngine>();
        m_watchdog = std::make_unique<ScriptWatchdog>(m_engine.get());
        m_network = std::make_unique<QNetworkAccessManager>();
        if (m_hooks.network)
            m_hooks.network(m_network.get());
        // Every callback into JS runs from an event this thread wakes for, so
        // arming on wake and disarming before sleep covers timers, replies,
        // socket messages and Promise continuations alike.
        auto *dispatcher = QAbstractEventDispatcher::instance();
        connect(dispatcher, &QAbstractEventDispatcher::awake, this, [this] {
            if (m_watchdog)
                m_watchdog->arm();
        });
        connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, this, [this] {
            if (m_watchdog) {
                m_watchdog->disarm();
                checkInterrupted();
            }
        });
        m_watchdog->arm();
        const QUrl entry(m_entryPoint);
        // importModule takes a file name: qrc:/x is :/x and file URLs are paths.
        const QString file = entry.scheme() == QStringLiteral("qrc") ? QLatin1Char(':') + entry.path()
            : entry.isLocalFile()                                    ? entry.toLocalFile()
                                                                     : m_entryPoint;
        m_module = m_engine->importModule(file);
        m_glue = m_engine->evaluate(QString::fromLatin1(kGlue));
        m_jsDevice = m_engine->toScriptValue(m_device);
        m_engine->globalObject()
            .property(QStringLiteral("Object"))
            .property(QStringLiteral("freeze"))
            .call({ m_jsDevice });
    }

    void checkInterrupted()
    {
        if (!m_engine || !m_engine->isInterrupted() || m_reportedInterrupt)
            return;
        m_reportedInterrupt = true;
        for (auto& source : m_sources) {
            for (ScriptOperation *operation : std::as_const(source.operations))
                operation->cancel("script_interrupted");
        }
        m_interrupted();
    }

    QString m_entryPoint;
    QVariantMap m_device;
    ScriptRuntime::NetworkHooks m_hooks;
    EventSink m_events;
    std::function<void()> m_interrupted;
    std::unique_ptr<QJSEngine> m_engine;
    std::unique_ptr<ScriptWatchdog> m_watchdog;
    std::unique_ptr<QNetworkAccessManager> m_network;
    QJSValue m_module;
    QJSValue m_glue;
    QJSValue m_jsDevice;
    QHash<QString, Source> m_sources;
    int m_activeOperations = 0;
    bool m_reportedInterrupt = false;
};

struct ScriptRuntime::Private {
    QThread thread;
    ScriptWorker *worker = nullptr;
    std::atomic<int> queued = 0;
    QHash<QString, quint64> sourceGenerations;
    quint64 nextGeneration = 0;

    template <typename T, typename Decoder>
    QCoro::Task<T> submit(QString sourceId, QString method, QVariantMap arguments, QString scope, Decoder decoder)
    {
        auto promise = std::make_shared<QPromise<T>>();
        promise->start();
        auto future = promise->future();
        if (queued.fetch_add(1) >= 64) {
            --queued;
            fail(promise, "queue_limit");
            return awaitResult(std::move(future));
        }
        auto sink = std::make_shared<TypedSink<T, Decoder>>(promise, std::move(decoder));
        QMetaObject::invokeMethod(
            worker,
            [this, sourceId = std::move(sourceId), method = std::move(method), arguments = std::move(arguments),
                scope = std::move(scope), sink] {
                --queued;
                worker->call(sourceId, method, arguments, scope, sink);
            },
            Qt::QueuedConnection);
        return awaitResult(std::move(future));
    }
};

ScriptRuntime::ScriptRuntime(QString entryPoint, QVariantMap device, NetworkHooks hooks, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
    d->worker = new ScriptWorker(
        std::move(entryPoint), std::move(device), std::move(hooks),
        [this](const QString& sourceId, quint64 generation, const QString& type, const QVariantMap& payload) {
            QMetaObject::invokeMethod(
                this,
                [this, sourceId, generation, type, payload] {
                    if (d->sourceGenerations.value(sourceId) == generation)
                        emit event(sourceId, type, payload);
                },
                Qt::QueuedConnection);
        },
        [this] { QMetaObject::invokeMethod(this, [this] { emit interrupted(); }, Qt::QueuedConnection); });
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

QCoro::Task<QVariantMap> ScriptRuntime::addSource(QString sourceId, QVariantMap configuration, QList<QUrl> origins,
    QVariantMap extensions, bool loginDraft, std::shared_ptr<std::atomic_bool> activationApproval)
{
    auto promise = std::make_shared<QPromise<QVariantMap>>();
    promise->start();
    auto future = promise->future();
    auto source = d->sourceGenerations.find(sourceId);
    if (source == d->sourceGenerations.end())
        source = d->sourceGenerations.insert(sourceId, ++d->nextGeneration);
    const quint64 generation = source.value();
    QMetaObject::invokeMethod(
        d->worker,
        [worker = d->worker, sourceId = std::move(sourceId), configuration = std::move(configuration),
            origins = std::move(origins), extensions = std::move(extensions), loginDraft,
            activationApproval = std::move(activationApproval), generation, promise] {
            worker->add(
                sourceId, configuration, origins, extensions, loginDraft, activationApproval, generation, promise);
        },
        Qt::QueuedConnection);
    return awaitResult(std::move(future));
}

QCoro::Task<void> ScriptRuntime::grantOrigins(
    QString sourceId, QList<QUrl> origins, std::shared_ptr<std::atomic_bool> approval)
{
    auto promise = std::make_shared<QPromise<bool>>();
    promise->start();
    auto future = promise->future();
    const quint64 generation = d->sourceGenerations.value(sourceId);
    QMetaObject::invokeMethod(
        d->worker,
        [worker = d->worker, sourceId = std::move(sourceId), origins = std::move(origins),
            approval = std::move(approval), generation,
            promise] { worker->grantOrigins(sourceId, generation, origins, approval, promise); },
        Qt::QueuedConnection);
    co_await awaitResult(std::move(future));
}

QCoro::Task<void> ScriptRuntime::allowLanDiscovery(QString sourceId)
{
    auto promise = std::make_shared<QPromise<bool>>();
    promise->start();
    auto future = promise->future();
    const quint64 generation = d->sourceGenerations.value(sourceId);
    QMetaObject::invokeMethod(
        d->worker,
        [worker = d->worker, sourceId = std::move(sourceId), generation, promise] {
            worker->allowLanDiscovery(sourceId, generation, promise);
        },
        Qt::QueuedConnection);
    co_await awaitResult(std::move(future));
}

void ScriptRuntime::cancelLanDiscovery(QString sourceId)
{
    const quint64 generation = d->sourceGenerations.value(sourceId);
    QMetaObject::invokeMethod(
        d->worker,
        [worker = d->worker, sourceId = std::move(sourceId), generation] {
            worker->cancelLanDiscovery(sourceId, generation);
        },
        Qt::QueuedConnection);
}

QCoro::Task<QVariantMap> ScriptRuntime::call(QString sourceId, QString method, QVariantMap arguments, QString scope)
{
    const bool data = method == QStringLiteral("dataRead");
    const bool strict = data || method == QStringLiteral("dataInfo") || method == QStringLiteral("dataWrite")
        || method == QStringLiteral("dataDelete") || method == QStringLiteral("preferencesRead")
        || method == QStringLiteral("preferencesWrite");
    return d->submit<QVariantMap>(std::move(sourceId), std::move(method), std::move(arguments), std::move(scope),
        [data, strict](const QJSValue& value) {
            if (strict
                && (!value.isObject() || value.isArray() || value.isNull() || value.isDate() || value.isRegExp()
                    || value.isError()))
                throw std::runtime_error("invalid_extension_result");
            return ownScriptValue(value, 50000, 4 * 1024 * 1024, data).toMap();
        });
}

QCoro::Task<ProviderMediaPage> ScriptRuntime::callMediaPage(
    QString sourceId, QString method, QVariantMap arguments, QString scope, int maximumItems)
{
    // Zero or less is a caller bug; the decoder rejects it.
    maximumItems = std::min(maximumItems, 1000);
    return d->submit<ProviderMediaPage>(std::move(sourceId), std::move(method), std::move(arguments), std::move(scope),
        [maximumItems](const QJSValue& value) { return Detail::readProviderMediaPage(value, maximumItems); });
}

QCoro::Task<MovieItem> ScriptRuntime::callItem(QString sourceId, QString method, QVariantMap arguments)
{
    return d->submit<MovieItem>(std::move(sourceId), std::move(method), std::move(arguments), QString(),
        [](const QJSValue& value) { return Detail::readProviderItem(value.property(QStringLiteral("item"))); });
}

void ScriptRuntime::cancelScope(const QString& sourceId, const QString& scope)
{
    QMetaObject::invokeMethod(
        d->worker, [worker = d->worker, sourceId, scope] { worker->cancelScope(sourceId, scope); },
        Qt::QueuedConnection);
}

void ScriptRuntime::removeSource(const QString& sourceId)
{
    d->sourceGenerations.remove(sourceId);
    QMetaObject::invokeMethod(
        d->worker, [worker = d->worker, sourceId] { worker->remove(sourceId); }, Qt::QueuedConnection);
}

} // namespace Spool
