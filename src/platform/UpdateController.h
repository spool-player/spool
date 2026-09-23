#pragma once

#include "app/UpdateManifest.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <memory>

class QCryptographicHash;
class QNetworkAccessManager;
class QNetworkReply;
class QSaveFile;
class QTemporaryDir;

namespace Spool {

#if defined(SPOOL_WEBOS)
class WebOSUpdateInstaller;
#endif

class UpdateController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString stage READ stage NOTIFY changed)
    Q_PROPERTY(QString version READ version NOTIFY changed)
    Q_PROPERTY(QString notes READ notes NOTIFY changed)
    Q_PROPERTY(QUrl releaseUrl READ releaseUrl NOTIFY changed)
    Q_PROPERTY(qint64 receivedBytes READ receivedBytes NOTIFY progressChanged)
    Q_PROPERTY(qint64 totalBytes READ totalBytes NOTIFY progressChanged)
    Q_PROPERTY(qint64 bytesPerSecond READ bytesPerSecond NOTIFY progressChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
    Q_PROPERTY(bool allowPrerelease READ allowPrerelease WRITE setAllowPrerelease NOTIFY allowPrereleaseChanged)

public:
    UpdateController(QNetworkAccessManager *network, QString cacheRoot, QObject *parent = nullptr);
    ~UpdateController() override;

    QString stage() const;
    QString version() const;
    QString notes() const;
    QUrl releaseUrl() const;
    qint64 receivedBytes() const;
    qint64 totalBytes() const;
    qint64 bytesPerSecond() const;
    double progress() const;
    QString errorText() const;
    bool allowPrerelease() const;
    void setAllowPrerelease(bool allow);

    // Whether the app checks for updates on its own. Settings owns the
    // value; this owns what it means. Turning it off mid-flight puts the
    // controller back to idle, so a dialog the user just opted out of does
    // not stay on screen.
    void setAutomaticUpdatesEnabled(bool enabled);
    Q_INVOKABLE void decline();
    Q_INVOKABLE void download();
    Q_INVOKABLE void cancelDownload();
    Q_INVOKABLE void install();
    Q_INVOKABLE void openInstallSettings();
    Q_INVOKABLE void retry();

signals:
    void changed();
    void progressChanged();
    void allowPrereleaseChanged();

private:
    enum class Stage {
        Idle,
        Checking,
        Available,
        Downloading,
        Ready,
        PermissionRequired,
        Installing,
        Installed,
        Error,
    };

    void checkForUpdate();
    void finishManifestRequest();
    void requestMetadata(const QUrl& url);
    void consumeMetadata();
    void offerRelease();
    void consumeDownloadData();
    void finishDownload();
    void setStage(Stage stage);
    void fail(QString message);
    void resetDownload();
    QString packagePath() const;

    QNetworkAccessManager *m_network = nullptr;
    QString m_cacheRoot;
    Stage m_stage = Stage::Idle;
    bool m_allowPrerelease = false;
    bool m_automaticUpdates = false;
    bool m_waitingForPermission = false;
    UpdateRelease m_release;
    QString m_errorText;
    QByteArray m_manifestBytes;
    QPointer<QNetworkReply> m_reply;
    std::unique_ptr<QSaveFile> m_output;
    std::unique_ptr<QCryptographicHash> m_hash;
    QElapsedTimer m_speedClock;
    QTimer m_speedTimer;
    qint64 m_receivedBytes = 0;
    qint64 m_totalBytes = 0;
    qint64 m_bytesPerSecond = 0;
    qint64 m_lastSpeedBytes = 0;
    qint64 m_lastSpeedMs = 0;
    bool m_packageReady = false;
#if defined(SPOOL_WEBOS)
    WebOSUpdateInstaller *m_installer = nullptr;
    std::unique_ptr<QTemporaryDir> m_packageDirectory;
#endif
};

} // namespace Spool
