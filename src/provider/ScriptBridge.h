#pragma once

// Worker-thread internals of ScriptRuntime: the objects provider JS calls
// back into. Nothing here is used outside ScriptRuntime.cpp and its tests.

#include <QHash>
#include <QJSEngine>
#include <QJSValue>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

class QNetworkAccessManager;
class QNetworkReply;
class QWebSocket;

namespace Spool {

// Interrupts JS that runs longer than 500 ms without returning to the event
// loop. Qt permits setInterrupted() from another thread.
class ScriptWatchdog {
public:
    explicit ScriptWatchdog(QJSEngine *engine);
    ~ScriptWatchdog();
    void arm();
    void disarm();

private:
    void run();
    QJSEngine *m_engine;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_stopping = false;
    bool m_armed = false;
    std::chrono::steady_clock::time_point m_deadline;
    std::thread m_thread;
};

// Converts a JS value to an owned native value in one bounded walk; also
// rejects cycles, functions and unsafe numbers.
QVariant ownScriptValue(const QJSValue& value, int maximumNodes = 50000, qsizetype maximumBytes = 4 * 1024 * 1024);

// Result delivery for one operation, type-erased per operation (not per row).
struct ScriptResultSink {
    virtual ~ScriptResultSink() = default;
    virtual void prepare(const QJSValue& value) = 0;
    virtual void complete() = 0;
    virtual void reject(const char *code) = 0;
};

// What every source may reach and how. Shared by a source's host and its
// operations; both check against the same origin list.
struct ScriptAccess {
    QJSEngine *engine = nullptr;
    QNetworkAccessManager *network = nullptr;
    QList<QUrl> origins;
    std::function<void(QWebSocket *, QUrl)> socketHook;
    bool allows(const QUrl& url) const;
};

class SpeedTest;

// Native requests a JS caller owns: HTTP, timers and LAN discovery, all
// cancelled together when the owner releases them.
class ScriptRequests final : public QObject {
    Q_OBJECT
public:
    ScriptRequests(ScriptAccess *access, QObject *parent);
    ~ScriptRequests() override;
    void http(const QString& address, const QVariantMap& options, QJSValue resolve, QJSValue reject);
    void speedTest(const QVariantMap& options, QJSValue resolve, QJSValue reject);
    void delay(int milliseconds, QJSValue resolve, QJSValue reject);
    void discover(int port, const QString& message, int timeoutMs, QJSValue resolve, QJSValue reject);
    void release();

signals:
    // A response exceeded the size limit; the owner fails as a whole.
    void overflowed();

private:
    ScriptAccess *m_access;
    QSet<QNetworkReply *> m_replies;
    QSet<QObject *> m_pending;
    SpeedTest *m_speedTest = nullptr;
};

// One provider operation: settles exactly once, within 15 seconds.
class ScriptOperation final : public QObject {
    Q_OBJECT
public:
    ScriptOperation(ScriptAccess *access, std::shared_ptr<ScriptResultSink> sink, QString scope, QObject *parent);
    ~ScriptOperation() override;
    const QString& scope() const
    {
        return m_scope;
    }
    void cancel(const char *code);
    Q_INVOKABLE void resolve(const QJSValue& result);
    Q_INVOKABLE void reject(const QJSValue& error);
    Q_INVOKABLE void http(const QString& url, const QVariantMap& options, QJSValue resolve, QJSValue reject);
    Q_INVOKABLE void speedTest(const QVariantMap& options, QJSValue resolve, QJSValue reject);
    Q_INVOKABLE void delay(int milliseconds, QJSValue resolve, QJSValue reject);
    Q_INVOKABLE void discover(int port, const QString& message, int timeoutMs, QJSValue resolve, QJSValue reject);

private:
    void release();
    ScriptAccess *m_access;
    std::shared_ptr<ScriptResultSink> m_sink;
    QString m_scope;
    ScriptRequests m_requests;
    QTimer m_deadline;
    bool m_settled = false;
};

// Services that live as long as a source: websockets, events and timers.
// This is how a provider keeps a server connection open for live updates,
// group playback and remote commands.
class ScriptSourceHost final : public QObject {
    Q_OBJECT
public:
    using EventSink = std::function<void(const QString& type, const QVariantMap& payload)>;
    ScriptSourceHost(ScriptAccess access, EventSink events, QObject *parent);
    ~ScriptSourceHost() override;
    ScriptAccess *access()
    {
        return &m_access;
    }
    Q_INVOKABLE void emitEvent(const QString& type, const QJSValue& payload);
    Q_INVOKABLE void http(const QString& url, const QVariantMap& options, QJSValue resolve, QJSValue reject);
    Q_INVOKABLE void delay(int milliseconds, QJSValue resolve, QJSValue reject);
    Q_INVOKABLE int socket(
        const QString& url, const QVariantMap& headers, QJSValue onOpen, QJSValue onMessage, QJSValue onClose);
    Q_INVOKABLE void socketSend(int id, const QString& text);
    Q_INVOKABLE void socketClose(int id);

private:
    ScriptAccess m_access;
    EventSink m_events;
    ScriptRequests m_requests;
    QHash<int, QPointer<QWebSocket>> m_sockets;
    int m_nextSocket = 1;
};

} // namespace Spool
