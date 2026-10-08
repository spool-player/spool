#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QLocalServer>
#include <QObject>
#include <QSet>
#include <QTcpServer>
#include <memory>

class QIODevice;
class QLockFile;

namespace Spool {
class AppController;
class DownloadManager;
class SourceHub;
class NativeAppWindow;
class RouterController;

class LocalCommandServer final : public QObject {
public:
    LocalCommandServer(AppController *app, RouterController *router, NativeAppWindow *window,
        DownloadManager *downloads, SourceHub *sources);
    ~LocalCommandServer() override;
    bool start(const QString& instance, QString *error);
    // Explicit opt-in only. Publishes the same private capability descriptor;
    // binds IPv4 loopback, including when port zero requests an ephemeral port.
    bool startTcp(const QString& instance, quint16 port, QString *error);
    void stop();

private:
    bool startInternal(const QString& instance, int tcpPort, QString *error);
    void acceptSocket(QIODevice *socket);
    void acceptConnections();
    void dispatch(QIODevice *socket, const QJsonObject& request);
    void finish(QIODevice *socket, const QString& id, const QJsonObject& result);
    void reject(QIODevice *socket, const QString& id, const QString& code, const QString& message);
    QJsonObject state() const;
    QJsonObject items(const QJsonObject& args) const;
    void screenshot(QIODevice *socket, const QString& id, const QString& path);

    AppController *m_app;
    RouterController *m_router;
    NativeAppWindow *m_window;
    DownloadManager *m_downloads;
    SourceHub *m_sources;
    QLocalServer m_server;
    QTcpServer m_tcpServer;
    QSet<QIODevice *> m_connections;
    std::unique_ptr<QLockFile> m_lock;
    QString m_descriptorPath;
    QString m_instance;
    QString m_token;
    bool m_settingPending = false;
    Qt::MouseButtons m_pointerButtons = Qt::NoButton;
    QElapsedTimer m_inputClock;
};
}
