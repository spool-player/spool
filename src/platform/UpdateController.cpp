#include "UpdateController.h"

#if defined(SPOOL_ANDROID)
#include "android/AndroidUpdateInstaller.h"
#elif defined(SPOOL_WEBOS)
#include "webos/WebOSUpdateInstaller.h"
#include <QTemporaryDir>
#endif

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QVersionNumber>

namespace Spool {

namespace {

#if defined(SPOOL_ANDROID)
    constexpr auto kManifestUrl = "https://spool-player.github.io/spool/updates/android.json";
    constexpr auto kAssetKey = SPOOL_ANDROID_ABI;
#else
    constexpr auto kManifestUrl = "https://spool-player.github.io/spool/updates/webos.json";
    constexpr auto kAssetKey = "arm";
#endif
    constexpr qsizetype kMaximumManifestBytes = 512 * 1024;
    constexpr qint64 kReadBufferBytes = 64 * 1024;

    int currentVersionCode()
    {
#if defined(SPOOL_ANDROID)
        return SPOOL_ANDROID_VERSION_CODE;
#else
        const QVersionNumber version = QVersionNumber::fromString(QStringLiteral(SPOOL_VERSION));
        return version.majorVersion() * 100000000 + version.minorVersion() * 100000 + version.microVersion() * 100 + 99;
#endif
    }

    bool trustedRedirect(const QUrl& url)
    {
        if (!url.isValid() || url.scheme() != QStringLiteral("https") || !url.userInfo().isEmpty()
            || (url.port() != -1 && url.port() != 443) || url.hasFragment())
            return false;
        const QString host = url.host().toLower();
        // GitHub redirects repository assets to these signed HTTPS storage URLs.
        if (host == QStringLiteral("release-assets.githubusercontent.com")
            || host == QStringLiteral("objects.githubusercontent.com"))
            return true;
        if (host == QStringLiteral("github.com"))
            return url.path().startsWith(QStringLiteral("/spool-player/spool/releases/download/"));
        return url == QUrl(QString::fromLatin1(kManifestUrl));
    }

    void configureRequest(QNetworkRequest& request, int timeout)
    {
#if defined(SPOOL_ANDROID)
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Spool-Android/%1").arg(SPOOL_VERSION));
#else
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Spool-webOS/%1").arg(SPOOL_VERSION));
#endif
        request.setTransferTimeout(timeout);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
        request.setMaximumRedirectsAllowed(5);
    }

    void restrictRedirects(QNetworkReply *reply)
    {
        reply->setReadBufferSize(kReadBufferBytes);
        QObject::connect(reply, &QNetworkReply::redirected, reply, [reply](const QUrl& url) {
            if (trustedRedirect(reply->url().resolved(url)))
                reply->redirectAllowed();
            else
                reply->abort();
        });
    }

}

UpdateController::UpdateController(QNetworkAccessManager *network, QString cacheRoot, QObject *parent)
    : QObject(parent)
    , m_network(network)
    , m_cacheRoot(std::move(cacheRoot))
{
    m_speedTimer.setInterval(500);
    connect(&m_speedTimer, &QTimer::timeout, this, [this] {
        const qint64 elapsedMs = m_speedClock.elapsed();
        const qint64 intervalMs = elapsedMs - m_lastSpeedMs;
        if (intervalMs <= 0)
            return;
        m_bytesPerSecond = (m_receivedBytes - m_lastSpeedBytes) * 1000 / intervalMs;
        m_lastSpeedBytes = m_receivedBytes;
        m_lastSpeedMs = elapsedMs;
        emit progressChanged();
    });
#if defined(SPOOL_ANDROID)
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive || !m_waitingForPermission)
            return;
        m_waitingForPermission = false;
        setStage(AndroidUpdateInstaller::canRequestPackageInstalls() ? Stage::Ready : Stage::PermissionRequired);
    });
#elif defined(SPOOL_WEBOS)
    m_installer = new WebOSUpdateInstaller(this);
    connect(m_installer, &WebOSUpdateInstaller::statusChanged, this,
        [this](const QString& message) { qInfo() << "update: webOS installer:" << message; });
    connect(m_installer, &WebOSUpdateInstaller::finished, this, [this] {
        if (m_stage != Stage::Installing)
            return;
        qInfo() << "update: webOS confirmed installation";
        if (m_packageDirectory)
            m_packageDirectory->setAutoRemove(true);
        setStage(Stage::Installed);
    });
    connect(m_installer, &WebOSUpdateInstaller::failed, this, [this](const QString& message) {
        if (m_stage != Stage::Installing)
            return;
        if (m_packageDirectory)
            m_packageDirectory->setAutoRemove(true);
        fail(message);
    });
#endif
}

UpdateController::~UpdateController()
{
    resetDownload();
}

QString UpdateController::stage() const
{
    switch (m_stage) {
    case Stage::Idle:
        return QStringLiteral("idle");
    case Stage::Checking:
        return QStringLiteral("checking");
    case Stage::Available:
        return QStringLiteral("available");
    case Stage::Downloading:
        return QStringLiteral("downloading");
    case Stage::Ready:
        return QStringLiteral("ready");
    case Stage::PermissionRequired:
        return QStringLiteral("permission");
    case Stage::Installing:
        return QStringLiteral("installing");
    case Stage::Installed:
        return QStringLiteral("installed");
    case Stage::Error:
        return QStringLiteral("error");
    }
    return QStringLiteral("idle");
}

QString UpdateController::version() const
{
    return m_release.version;
}
QString UpdateController::notes() const
{
    return m_release.notes;
}
QUrl UpdateController::releaseUrl() const
{
    return m_release.releaseUrl;
}
qint64 UpdateController::receivedBytes() const
{
    return m_receivedBytes;
}
qint64 UpdateController::totalBytes() const
{
    return m_totalBytes;
}
qint64 UpdateController::bytesPerSecond() const
{
    return m_bytesPerSecond;
}
QString UpdateController::errorText() const
{
    return m_errorText;
}
bool UpdateController::allowPrerelease() const
{
    return m_allowPrerelease;
}

double UpdateController::progress() const
{
    return m_totalBytes > 0 ? qBound(0.0, static_cast<double>(m_receivedBytes) / m_totalBytes, 1.0) : 0.0;
}

void UpdateController::setAllowPrerelease(bool allow)
{
    if (m_allowPrerelease == allow)
        return;
    m_allowPrerelease = allow;
    emit allowPrereleaseChanged();
}

void UpdateController::setAutomaticUpdatesEnabled(bool enabled)
{
    if (m_automaticUpdates == enabled)
        return;
    m_automaticUpdates = enabled;
    qInfo() << "update: automatic updates" << (enabled ? "enabled" : "disabled");
    if (!enabled) {
        decline();
        return;
    }
    if (m_stage == Stage::Idle)
        checkForUpdate();
}

void UpdateController::checkForUpdate()
{
    if (!m_network || m_reply || m_stage == Stage::Installing)
        return;
    m_release = {};
    m_errorText.clear();
    setStage(Stage::Checking);
    requestMetadata(QUrl(QString::fromLatin1(kManifestUrl)));
}

void UpdateController::requestMetadata(const QUrl& url)
{
    m_manifestBytes.clear();
    QNetworkRequest request(url);
    configureRequest(request, 15000);
    request.setRawHeader("Accept", "application/json");
    m_reply = m_network->get(request);
    restrictRedirects(m_reply);
    connect(m_reply, &QNetworkReply::readyRead, this, &UpdateController::consumeMetadata);
    connect(m_reply, &QNetworkReply::finished, this, &UpdateController::finishManifestRequest);
}

void UpdateController::consumeMetadata()
{
    if (!m_reply)
        return;
    while (m_reply->bytesAvailable() > 0) {
        const QByteArray bytes
            = m_reply->read(qMin<qint64>(kReadBufferBytes, kMaximumManifestBytes - m_manifestBytes.size() + 1));
        if (bytes.isEmpty())
            break;
        m_manifestBytes += bytes;
        if (m_manifestBytes.size() > kMaximumManifestBytes) {
            resetDownload();
            m_manifestBytes.clear();
            qWarning() << "update: metadata exceeds the safe size limit";
            setStage(Stage::Idle);
            return;
        }
    }
}

void UpdateController::finishManifestRequest()
{
    consumeMetadata();
    if (!m_reply)
        return;
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool validResponse = reply->error() == QNetworkReply::NoError && status == 200;
    const QString networkError = reply->errorString();
    reply->deleteLater();
    if (!m_automaticUpdates || m_stage != Stage::Checking)
        return;
    if (!validResponse) {
        qWarning() << "update check failed:" << status << networkError;
        setStage(Stage::Idle);
        return;
    }

    UpdateManifestResult result
        = selectUpdate(m_manifestBytes, currentVersionCode(), m_allowPrerelease, QString::fromLatin1(kAssetKey));
    m_manifestBytes.clear();
    if (!result.error.isEmpty()) {
        qWarning() << "update manifest rejected:" << result.error;
        setStage(Stage::Idle);
        return;
    }
    if (!result.release) {
        setStage(Stage::Idle);
        return;
    }
    m_release = std::move(*result.release);
    offerRelease();
}

void UpdateController::offerRelease()
{
    m_totalBytes = m_release.packageSize;
    m_receivedBytes = 0;
    m_bytesPerSecond = 0;
    emit progressChanged();
    setStage(Stage::Available);
}

QString UpdateController::packagePath() const
{
#if defined(SPOOL_WEBOS)
    return m_packageDirectory ? m_packageDirectory->filePath(QStringLiteral("spool-update.ipk")) : QString();
#else
    return QDir(m_cacheRoot).filePath(QStringLiteral("updates/spool-update.apk"));
#endif
}

void UpdateController::decline()
{
    if (m_stage == Stage::Installing)
        return;
    // Disconnect before abort: a cancelled metadata request must never reopen the dialog.
    resetDownload();
    m_manifestBytes.clear();
    m_waitingForPermission = false;
    if (m_packageReady) {
        QFile::remove(packagePath());
        m_packageReady = false;
    }
#if defined(SPOOL_WEBOS)
    m_packageDirectory.reset();
#endif
    m_errorText.clear();
    setStage(Stage::Idle);
}

void UpdateController::download()
{
    if (!m_network || (m_stage != Stage::Available && m_stage != Stage::Error) || m_release.packageUrl.isEmpty()
        || m_release.packageSha256.isEmpty())
        return;
    resetDownload();
    m_packageReady = false;
    m_errorText.clear();
#if defined(SPOOL_WEBOS)
    // Only this application's freshly created directory is made traversable.
    // /tmp avoids permissions on app-cache ancestors owned by another service.
    m_packageDirectory = std::make_unique<QTemporaryDir>(QStringLiteral("/tmp/spool-update-XXXXXX"));
    constexpr auto directoryPermissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
        | QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther;
    if (!m_packageDirectory->isValid() || !QFile::setPermissions(m_packageDirectory->path(), directoryPermissions)) {
#else
    if (!QDir().mkpath(QDir(m_cacheRoot).filePath(QStringLiteral("updates")))) {
#endif
        fail(QStringLiteral("Could not create a safe location for the update."));
        return;
    }
    m_output = std::make_unique<QSaveFile>(packagePath());
    if (!m_output->open(QIODevice::WriteOnly)) {
        fail(QStringLiteral("Could not create the update file."));
        return;
    }
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    m_receivedBytes = 0;
    m_totalBytes = m_release.packageSize;
    m_bytesPerSecond = 0;
    m_lastSpeedBytes = 0;
    m_lastSpeedMs = 0;
    m_speedClock.start();
    m_speedTimer.start();
    emit progressChanged();
    setStage(Stage::Downloading);

    QNetworkRequest request(m_release.packageUrl);
    configureRequest(request, 30000);
    request.setRawHeader("Accept", "application/octet-stream");
    m_reply = m_network->get(request);
    restrictRedirects(m_reply);
    connect(m_reply, &QNetworkReply::readyRead, this, &UpdateController::consumeDownloadData);
    connect(m_reply, &QNetworkReply::finished, this, &UpdateController::finishDownload);
}

void UpdateController::consumeDownloadData()
{
    if (!m_reply || !m_output || !m_hash)
        return;
    while (m_reply->bytesAvailable() > 0) {
        const QByteArray bytes = m_reply->read(kReadBufferBytes);
        if (bytes.isEmpty())
            break;
        if (bytes.size() > m_release.packageSize - m_receivedBytes) {
            resetDownload();
            fail(QStringLiteral("The downloaded package exceeds its expected size and was discarded."));
            return;
        }
        if (m_output->write(bytes) != bytes.size()) {
            resetDownload();
            fail(QStringLiteral("The update file could not be written."));
            return;
        }
        m_hash->addData(bytes);
        m_receivedBytes += bytes.size();
    }
    emit progressChanged();
}

void UpdateController::finishDownload()
{
    consumeDownloadData();
    if (!m_reply)
        return;
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    m_speedTimer.stop();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool networkOk = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
    const QString networkError = reply->errorString();
    reply->deleteLater();

    const bool sizeOk = m_receivedBytes == m_release.packageSize;
    const bool hashOk = m_hash && m_hash->result().toHex() == m_release.packageSha256;
    if (!networkOk || !sizeOk || !hashOk || !m_output || !m_output->commit()) {
        resetDownload();
        if (!networkOk)
            fail(QStringLiteral("The update download failed: %1").arg(networkError));
        else if (!sizeOk)
            fail(QStringLiteral("The downloaded package has the wrong size and was discarded."));
        else if (!hashOk)
            fail(QStringLiteral("The downloaded package failed its SHA-256 check and was discarded."));
        else
            fail(QStringLiteral("The verified update could not be saved."));
        return;
    }

    m_output.reset();
    m_hash.reset();
#if defined(SPOOL_WEBOS)
    constexpr auto packagePermissions
        = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther;
    if (!QFile::setPermissions(packagePath(), packagePermissions)) {
        QFile::remove(packagePath());
        fail(QStringLiteral("The verified IPK could not be made readable by the installer service."));
        return;
    }
#endif
    m_bytesPerSecond = 0;
    emit progressChanged();
    m_packageReady = true;
    setStage(Stage::Ready);
}

void UpdateController::cancelDownload()
{
    if (m_stage != Stage::Downloading)
        return;
    resetDownload();
    m_receivedBytes = 0;
    m_bytesPerSecond = 0;
    emit progressChanged();
    setStage(Stage::Available);
}

void UpdateController::install()
{
    if (!m_packageReady || (m_stage != Stage::Ready && m_stage != Stage::PermissionRequired))
        return;
#if defined(SPOOL_ANDROID)
    if (!AndroidUpdateInstaller::canRequestPackageInstalls()) {
        setStage(Stage::PermissionRequired);
        return;
    }
    if (!AndroidUpdateInstaller::install(packagePath()))
        fail(QStringLiteral("Android could not open the package installer."));
#elif defined(SPOOL_WEBOS)
    if (m_installer->isRunning())
        return;
    // The service may terminate this process to replace the application. Do not
    // remove the file during application teardown while the service still uses it.
    m_packageDirectory->setAutoRemove(false);
    setStage(Stage::Installing);
    m_installer->install(packagePath());
#endif
}

void UpdateController::openInstallSettings()
{
#if defined(SPOOL_ANDROID)
    if (m_stage != Stage::PermissionRequired)
        return;
    if (!AndroidUpdateInstaller::openInstallSettings()) {
        fail(QStringLiteral("Android could not open the ‘Install unknown apps’ setting."));
        return;
    }
    m_waitingForPermission = true;
#endif
}

void UpdateController::retry()
{
    if (m_stage != Stage::Error)
        return;
    if (m_packageReady) {
        m_errorText.clear();
        setStage(Stage::Ready);
        return;
    }
    if (!m_release.packageUrl.isEmpty() && !m_release.packageSha256.isEmpty())
        download();
    else
        checkForUpdate();
}

void UpdateController::setStage(Stage stage)
{
    if (m_stage == stage)
        return;
    m_stage = stage;
    emit changed();
}

void UpdateController::fail(QString message)
{
    m_errorText = std::move(message);
    qWarning() << "update:" << m_errorText;
    setStage(Stage::Error);
}

void UpdateController::resetDownload()
{
    m_speedTimer.stop();
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    if (m_output)
        m_output->cancelWriting();
    m_output.reset();
    m_hash.reset();
}

} // namespace Spool
