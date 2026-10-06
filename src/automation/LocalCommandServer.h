#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QLocalServer>
#include <QObject>
#include <QSet>
#include <memory>

class QLocalSocket;
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
    void stop();

private:
    void acceptConnections();
    void dispatch(QLocalSocket *socket, const QJsonObject& request);
    void finish(QLocalSocket *socket, const QString& id, const QJsonObject& result);
    void reject(QLocalSocket *socket, const QString& id, const QString& code, const QString& message);
    QJsonObject state() const;
    QJsonObject items(const QJsonObject& args) const;
    void screenshot(QLocalSocket *socket, const QString& id, const QString& path);

    AppController *m_app;
    RouterController *m_router;
    NativeAppWindow *m_window;
    DownloadManager *m_downloads;
    SourceHub *m_sources;
    QLocalServer m_server;
    QSet<QLocalSocket *> m_connections;
    std::unique_ptr<QLockFile> m_lock;
    QString m_descriptorPath;
    QString m_instance;
    QString m_token;
    bool m_settingPending = false;
    Qt::MouseButtons m_pointerButtons = Qt::NoButton;
    QElapsedTimer m_inputClock;
};
}
