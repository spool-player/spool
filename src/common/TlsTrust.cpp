#include "TlsTrust.h"

#include <QCryptographicHash>
#include <QDir>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QSaveFile>
#include <QSettings>
#include <QSslConfiguration>
#include <QStandardPaths>
#include <QThread>
#include <QWebSocket>

#include <algorithm>

namespace Spool {

namespace {

    QString joinedErrors(const QList<QSslError>& errors)
    {
        QStringList messages;
        for (const QSslError& error : errors) {
            const QString message = error.errorString().trimmed();
            if (!message.isEmpty() && !messages.contains(message))
                messages.push_back(message);
        }
        return messages.join(QStringLiteral("\n"));
    }

    // The remembered certificates, read straight from settings rather than
    // through a controller: the bundle is written before any of the UI exists.
    QList<QSslCertificate> rememberedCertificates()
    {
        QList<QSslCertificate> certificates;
        QSettings settings;
        settings.beginGroup(QStringLiteral("tls/trusted"));
        const QStringList groups = settings.childGroups();
        for (const QString& group : groups) {
            settings.beginGroup(group);
            const QByteArray storedFingerprint = settings.value(QStringLiteral("fingerprint")).toByteArray().toLower();
            const QByteArray pem = settings.value(QStringLiteral("certificatePem")).toByteArray();
            settings.endGroup();
            const QSslCertificate certificate(pem, QSsl::Pem);
            // Same check the controller makes before trusting one: a settings
            // file edited by hand must not widen what playback accepts.
            if (!certificate.isNull() && TlsTrustController::fingerprint(certificate) == storedFingerprint)
                certificates.push_back(certificate);
        }
        settings.endGroup();
        return certificates;
    }

} // namespace

QString writeTrustBundle()
{
    QList<QSslCertificate> certificates = QSslConfiguration::defaultConfiguration().caCertificates();
    certificates += rememberedCertificates();
    if (certificates.isEmpty())
        return {};

    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (directory.isEmpty() || !QDir().mkpath(directory))
        return {};

    const QString path = QDir(directory).filePath(QStringLiteral("ca-bundle.pem"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    for (const QSslCertificate& certificate : std::as_const(certificates)) {
        const QByteArray pem = certificate.toPem();
        if (!pem.isEmpty() && file.write(pem) != pem.size())
            return {};
    }
    // Rewritten every launch so a certificate the viewer forgets, or a platform
    // store that changes under the app, does not linger in playback's trust.
    if (!file.commit())
        return {};
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return path;
}

TlsTrustController::TlsTrustController(QObject *parent)
    : QObject(parent)
{
    reloadRemembered();
}

bool TlsTrustController::pending() const
{
    return m_current.has_value();
}

QString TlsTrustController::pendingAuthority() const
{
    return m_current ? authority(m_current->url) : QString();
}

QString TlsTrustController::pendingFingerprint() const
{
    return m_current ? displayFingerprint(m_current->certificate) : QString();
}

QString TlsTrustController::pendingIssuer() const
{
    return m_current ? m_current->certificate.issuerDisplayName() : QString();
}

QString TlsTrustController::pendingErrors() const
{
    return m_current ? joinedErrors(m_current->errors) : QString();
}

QString TlsTrustController::pendingSource() const
{
    return m_current ? m_current->source : QString();
}

QVariantList TlsTrustController::rememberedCertificates() const
{
    QMutexLocker locker(&m_mutex);
    return m_rememberedCertificates;
}

void TlsTrustController::attachNetworkAccessManager(QNetworkAccessManager *manager, QString source)
{
    if (!manager || manager->property("spoolTlsTrustAttached").toBool())
        return;
    manager->setProperty("spoolTlsTrustAttached", true);
    connect(
        manager, &QNetworkAccessManager::sslErrors, this,
        [this, source = std::move(source)](
            QNetworkReply *reply, const QList<QSslError>& errors) { handleNetworkErrors(reply, errors, source); },
        Qt::DirectConnection);
}

void TlsTrustController::attachWebSocket(QWebSocket *socket, std::function<QUrl()> urlProvider, QString source)
{
    if (!socket)
        return;
    connect(
        socket, &QWebSocket::sslErrors, this,
        [this, socket, urlProvider = std::move(urlProvider), source = std::move(source)](
            const QList<QSslError>& errors) {
            QSslCertificate certificate = socket->sslConfiguration().peerCertificate();
            if (certificate.isNull()) {
                for (const QSslError& error : errors) {
                    if (!error.certificate().isNull()) {
                        certificate = error.certificate();
                        break;
                    }
                }
            }
            handleSocketErrors(socket, urlProvider ? urlProvider() : QUrl(), certificate, errors, source);
        },
        Qt::DirectConnection);
}

bool TlsTrustController::isTrusted(const QUrl& url, const QSslCertificate& certificate) const
{
    if (certificate.isNull())
        return false;
    const QString key = endpointKey(url);
    QMutexLocker locker(&m_mutex);
    return !m_fingerprints.value(key).isEmpty() && m_fingerprints.value(key) == fingerprint(certificate);
}

QSslCertificate TlsTrustController::trustedCertificate(const QUrl& url) const
{
    const QString key = endpointKey(url);
    QByteArray pem;
    QByteArray expected;
    {
        QMutexLocker locker(&m_mutex);
        pem = m_certificates.value(key);
        expected = m_fingerprints.value(key);
    }
    const QSslCertificate certificate(pem, QSsl::Pem);
    return !certificate.isNull() && fingerprint(certificate) == expected ? certificate : QSslCertificate();
}

void TlsTrustController::rememberCertificate(const QUrl& url, const QSslCertificate& certificate)
{
    if (url.host().isEmpty() || certificate.isNull())
        return;
    const QString key = endpointKey(url);
    const QByteArray digest = fingerprint(certificate);
    QSettings settings;
    settings.setValue(key + QStringLiteral("/authority"), authority(url));
    settings.setValue(key + QStringLiteral("/fingerprint"), digest);
    settings.setValue(key + QStringLiteral("/certificatePem"), certificate.toPem());
    settings.setValue(key + QStringLiteral("/issuer"), certificate.issuerDisplayName());
    settings.sync();
    reloadRemembered();
    emit rememberedCertificatesChanged();
}

QSslCertificate TlsTrustController::peerCertificate(QNetworkReply *reply, const QList<QSslError>& errors)
{
    if (reply) {
        const QSslCertificate certificate = reply->sslConfiguration().peerCertificate();
        if (!certificate.isNull())
            return certificate;
    }
    for (const QSslError& error : errors) {
        if (!error.certificate().isNull())
            return error.certificate();
    }
    return QSslCertificate();
}

QByteArray TlsTrustController::fingerprint(const QSslCertificate& certificate)
{
    return certificate.digest(QCryptographicHash::Sha256).toHex().toLower();
}

QString TlsTrustController::displayFingerprint(const QSslCertificate& certificate)
{
    const QByteArray compact = fingerprint(certificate).toUpper();
    QStringList groups;
    groups.reserve((compact.size() + 1) / 2);
    for (qsizetype index = 0; index < compact.size(); index += 2)
        groups.push_back(QString::fromLatin1(compact.mid(index, 2)));
    return groups.join(QLatin1Char(':'));
}

QString TlsTrustController::authority(const QUrl& url)
{
    QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("wss"))
        scheme = QStringLiteral("https");
    else if (scheme == QStringLiteral("ws"))
        scheme = QStringLiteral("http");
    const int port = url.port(scheme == QStringLiteral("https") ? 443 : -1);
    return scheme + QStringLiteral("://") + url.host().toLower() + QLatin1Char(':') + QString::number(port);
}

QString TlsTrustController::endpointKey(const QUrl& url)
{
    const QByteArray endpoint = authority(url).toUtf8();
    return QStringLiteral("tls/trusted/")
        + QString::fromLatin1(QCryptographicHash::hash(endpoint, QCryptographicHash::Sha256).toHex());
}

void TlsTrustController::trustOnce()
{
    resolveCurrent(true, false);
}

void TlsTrustController::remember()
{
    resolveCurrent(true, true);
}

void TlsTrustController::cancel()
{
    resolveCurrent(false, false);
}

void TlsTrustController::removeRemembered(const QString& key)
{
    if (!key.startsWith(QStringLiteral("tls/trusted/")))
        return;
    QSettings settings;
    settings.remove(key);
    settings.sync();
    reloadRemembered();
    emit rememberedCertificatesChanged();
}

void TlsTrustController::handleNetworkErrors(
    QNetworkReply *reply, const QList<QSslError>& errors, const QString& source)
{
    if (!reply)
        return;
    const QSslCertificate certificate = peerCertificate(reply, errors);
    if (certificate.isNull())
        return;
    if (isTrusted(reply->url(), certificate)) {
        reply->ignoreSslErrors(errors);
        return;
    }
    {
        QMutexLocker locker(&m_mutex);
        const QString key = endpointKey(reply->url());
        const QByteArray onceFingerprint = m_onceFingerprints.value(key);
        if (!onceFingerprint.isEmpty() && onceFingerprint == fingerprint(certificate)) {
            m_onceFingerprints.remove(key);
            reply->ignoreSslErrors(errors);
            return;
        }
    }
    Decision decision { reply->url(), certificate, errors, reply, nullptr, source };
    if (thread() == QThread::currentThread()) {
        enqueueDecision(std::move(decision));
    } else {
        QMetaObject::invokeMethod(
            this, [this, decision = std::move(decision)]() mutable { enqueueDecision(std::move(decision)); });
    }
}

void TlsTrustController::handleSocketErrors(QWebSocket *socket, const QUrl& url, const QSslCertificate& certificate,
    const QList<QSslError>& errors, const QString& source)
{
    if (!socket || certificate.isNull())
        return;
    if (isTrusted(url, certificate)) {
        socket->ignoreSslErrors(errors);
        return;
    }
    {
        QMutexLocker locker(&m_mutex);
        const QString key = endpointKey(url);
        const QByteArray onceFingerprint = m_onceFingerprints.value(key);
        if (!onceFingerprint.isEmpty() && onceFingerprint == fingerprint(certificate)) {
            m_onceFingerprints.remove(key);
            socket->ignoreSslErrors(errors);
            return;
        }
    }
    Decision decision { url, certificate, errors, nullptr, socket, source };
    if (thread() == QThread::currentThread()) {
        enqueueDecision(std::move(decision));
    } else {
        QMetaObject::invokeMethod(
            this, [this, decision = std::move(decision)]() mutable { enqueueDecision(std::move(decision)); });
    }
}

void TlsTrustController::enqueueDecision(Decision decision)
{
    if (isTrusted(decision.url, decision.certificate)) {
        allowDecision(decision);
        return;
    }
    const auto duplicate = [&decision](const Decision& existing) {
        return endpointKey(existing.url) == endpointKey(decision.url)
            && fingerprint(existing.certificate) == fingerprint(decision.certificate)
            && existing.reply == decision.reply && existing.socket == decision.socket;
    };
    if ((m_current && duplicate(*m_current)) || std::any_of(m_queue.cbegin(), m_queue.cend(), duplicate))
        return;
    m_queue.enqueue(std::move(decision));
    advance();
}

void TlsTrustController::resolveCurrent(bool trusted, bool remembered)
{
    if (!m_current)
        return;
    const Decision decision = *m_current;
    m_current.reset();
    if (trusted && !remembered) {
        QMutexLocker locker(&m_mutex);
        m_onceFingerprints.insert(endpointKey(decision.url), fingerprint(decision.certificate));
    }
    if (trusted && remembered)
        rememberCertificate(decision.url, decision.certificate);
    if (trusted)
        allowDecision(decision);
    else
        rejectDecision(decision);
    emit decisionResolved(decision.source, trusted, remembered);

    if (trusted && remembered) {
        QQueue<Decision> retained;
        while (!m_queue.isEmpty()) {
            const Decision queued = m_queue.dequeue();
            if (endpointKey(queued.url) == endpointKey(decision.url)
                && fingerprint(queued.certificate) == fingerprint(decision.certificate)) {
                allowDecision(queued);
                emit decisionResolved(queued.source, true, true);
            } else {
                retained.enqueue(queued);
            }
        }
        m_queue = std::move(retained);
    }
    emit pendingChanged();
    advance();
}

void TlsTrustController::allowDecision(const Decision& decision)
{
    if (decision.reply) {
        const QPointer<QNetworkReply> reply = decision.reply;
        const QList<QSslError> errors = decision.errors;
        QMetaObject::invokeMethod(reply, [reply, errors]() {
            if (reply)
                reply->ignoreSslErrors(errors);
        });
    }
    if (decision.socket) {
        const QPointer<QWebSocket> socket = decision.socket;
        const QList<QSslError> errors = decision.errors;
        QMetaObject::invokeMethod(socket, [socket, errors]() {
            if (socket)
                socket->ignoreSslErrors(errors);
        });
    }
}

void TlsTrustController::rejectDecision(const Decision& decision)
{
    if (decision.reply) {
        const QPointer<QNetworkReply> reply = decision.reply;
        QMetaObject::invokeMethod(reply, [reply]() {
            if (reply)
                reply->abort();
        });
    }
    if (decision.socket) {
        const QPointer<QWebSocket> socket = decision.socket;
        QMetaObject::invokeMethod(socket, [socket]() {
            if (socket)
                socket->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("TLS trust cancelled"));
        });
    }
}

void TlsTrustController::advance()
{
    if (m_current || m_queue.isEmpty())
        return;
    m_current = m_queue.dequeue();
    qWarning().noquote() << "TLS trust decision required" << authority(m_current->url)
                         << displayFingerprint(m_current->certificate) << joinedErrors(m_current->errors)
                         << m_current->certificate.issuerDisplayName();
    emit pendingChanged();
}

void TlsTrustController::reloadRemembered()
{
    QHash<QString, QByteArray> fingerprints;
    QHash<QString, QByteArray> certificates;
    QVariantList entries;
    QSettings settings;
    settings.beginGroup(QStringLiteral("tls/trusted"));
    const QStringList groups = settings.childGroups();
    for (const QString& group : groups) {
        settings.beginGroup(group);
        const QByteArray storedFingerprint = settings.value(QStringLiteral("fingerprint")).toByteArray().toLower();
        const QByteArray certificatePem = settings.value(QStringLiteral("certificatePem")).toByteArray();
        const QString storedAuthority = settings.value(QStringLiteral("authority")).toString();
        const QString issuer = settings.value(QStringLiteral("issuer")).toString();
        settings.endGroup();
        const QSslCertificate certificate(certificatePem, QSsl::Pem);
        if (storedAuthority.isEmpty() || certificate.isNull() || storedFingerprint != fingerprint(certificate))
            continue;
        const QString key = QStringLiteral("tls/trusted/") + group;
        fingerprints.insert(key, storedFingerprint);
        certificates.insert(key, certificatePem);
        entries.push_back(QVariantMap { { QStringLiteral("key"), key },
            { QStringLiteral("authority"), storedAuthority },
            { QStringLiteral("fingerprint"), displayFingerprint(certificate) }, { QStringLiteral("issuer"), issuer } });
    }
    settings.endGroup();
    std::sort(entries.begin(), entries.end(), [](const QVariant& left, const QVariant& right) {
        return left.toMap().value(QStringLiteral("authority")).toString()
            < right.toMap().value(QStringLiteral("authority")).toString();
    });
    QMutexLocker locker(&m_mutex);
    m_fingerprints = std::move(fingerprints);
    m_certificates = std::move(certificates);
    m_rememberedCertificates = std::move(entries);
}

} // namespace Spool
