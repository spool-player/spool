#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSslCertificate>
#include <QSslError>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include <functional>
#include <optional>

#if !QT_CONFIG(ssl)
#error "Spool requires a Qt Network build with TLS support"
#endif

class QNetworkAccessManager;
class QWebSocket;

namespace Spool {

// Writes everything the app trusts -- the platform store Qt resolves, plus any
// certificate the viewer chose to remember -- to a PEM file and returns its
// path, or an empty string if it could not be written.
//
// libcurl carries playback and does not share Qt's trust. Left to itself it
// falls back to a CA location chosen when curl was compiled, which is a
// standing guess about the device: Android 14 moved the system store into the
// Conscrypt APEX, so a build naming /system/etc/security/cacerts verifies
// nothing on a current phone even though Qt, which asks Java, is fine. Reading
// the answer out of Qt keeps one trust store for the whole app, and is what
// lets a remembered self-signed certificate reach playback at all.
QString writeTrustBundle();

class TlsTrustController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool pending READ pending NOTIFY pendingChanged)
    Q_PROPERTY(QString pendingAuthority READ pendingAuthority NOTIFY pendingChanged)
    Q_PROPERTY(QString pendingFingerprint READ pendingFingerprint NOTIFY pendingChanged)
    Q_PROPERTY(QString pendingIssuer READ pendingIssuer NOTIFY pendingChanged)
    Q_PROPERTY(QString pendingErrors READ pendingErrors NOTIFY pendingChanged)
    Q_PROPERTY(QString pendingSource READ pendingSource NOTIFY pendingChanged)
    Q_PROPERTY(QVariantList rememberedCertificates READ rememberedCertificates NOTIFY rememberedCertificatesChanged)

public:
    explicit TlsTrustController(QObject *parent = nullptr);

    bool pending() const;
    QString pendingAuthority() const;
    QString pendingFingerprint() const;
    QString pendingIssuer() const;
    QString pendingErrors() const;
    QString pendingSource() const;
    QVariantList rememberedCertificates() const;

    void attachNetworkAccessManager(QNetworkAccessManager *manager, QString source);
    void attachWebSocket(QWebSocket *socket, std::function<QUrl()> urlProvider, QString source);

    bool isTrusted(const QUrl& url, const QSslCertificate& certificate) const;
    QSslCertificate trustedCertificate(const QUrl& url) const;
    void rememberCertificate(const QUrl& url, const QSslCertificate& certificate);

    static QSslCertificate peerCertificate(QNetworkReply *reply, const QList<QSslError>& errors);
    static QByteArray fingerprint(const QSslCertificate& certificate);
    static QString displayFingerprint(const QSslCertificate& certificate);
    static QString authority(const QUrl& url);
    static QString endpointKey(const QUrl& url);

    Q_INVOKABLE void trustOnce();
    Q_INVOKABLE void remember();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void removeRemembered(const QString& key);

signals:
    void pendingChanged();
    void rememberedCertificatesChanged();
    void decisionResolved(const QString& source, bool trusted, bool remembered);

private:
    struct Decision {
        QUrl url;
        QSslCertificate certificate;
        QList<QSslError> errors;
        QPointer<QNetworkReply> reply;
        QPointer<QWebSocket> socket;
        QString source;
    };

    void handleNetworkErrors(QNetworkReply *reply, const QList<QSslError>& errors, const QString& source);
    void handleSocketErrors(QWebSocket *socket, const QUrl& url, const QSslCertificate& certificate,
        const QList<QSslError>& errors, const QString& source);
    void enqueueDecision(Decision decision);
    void resolveCurrent(bool trusted, bool remembered);
    void allowDecision(const Decision& decision);
    void rejectDecision(const Decision& decision);
    void advance();
    void reloadRemembered();

    mutable QMutex m_mutex;
    QHash<QString, QByteArray> m_fingerprints;
    QHash<QString, QByteArray> m_certificates;
    QHash<QString, QByteArray> m_onceFingerprints;
    QVariantList m_rememberedCertificates;
    std::optional<Decision> m_current;
    QQueue<Decision> m_queue;
};

} // namespace Spool
