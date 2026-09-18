#pragma once

#include "DiscoveredServer.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QSslCertificate>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>

namespace JellyfinNative {

class TlsTrustController;
class DiscoveryController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(bool serverProbeActive READ serverProbeActive NOTIFY serverProbeActiveChanged)

public:
    explicit DiscoveryController(TlsTrustController *tlsTrust, QObject *parent = nullptr);
    ~DiscoveryController() override;

    bool active() const;
    bool serverProbeActive() const;

    static QList<QUrl> serverProbeCandidates(const QString& input);
    // True once the typed text names an address worth probing on its own, so
    // the login screen can connect as you type instead of waiting for Enter.
    Q_INVOKABLE static bool looksLikeServerAddress(const QString& input);
    static QList<QHostAddress> httpFallbackTargets(
        const QHostAddress& address, const QHostAddress& netmask, int maxTargets = 254);
    static DiscoveredServer serverFromPublicInfo(const QByteArray& payload, const QUrl& serverUrl, QString *version);

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void probeServer(const QString& input);
    Q_INVOKABLE void cancelServerProbe();

signals:
    void activeChanged();
    void serverProbeActiveChanged();
    void serverDiscovered(const JellyfinNative::DiscoveredServer& server);
    void serverProbeSucceeded(
        const QString& input, const JellyfinNative::DiscoveredServer& server, const QString& version, bool plainHttp);
    void serverProbeFailed(const QString& input, const QString& message);

private slots:
    void sendProbe();
    void handlePendingDatagrams();
    void startHttpFallbackScan();

private:
    bool ensureSocket();
    void sendUnicastSweep();
    void pumpUnicastSweep();
    void enqueueHttpProbeTarget(const QHostAddress& address);
    void pumpHttpProbeQueue();
    void handleHttpProbeResult(const QString& serverUrl, const QByteArray& payload);
    void startNextServerProbe();
    void finishServerProbe(bool notifyFailure);

    QUdpSocket m_socket;
    QTimer m_rescanTimer;
    QTimer m_unicastSweepTimer;
    QQueue<QHostAddress> m_unicastSweepQueue;
    QNetworkAccessManager m_http;
    QQueue<QHostAddress> m_httpProbeQueue;
    QSet<QString> m_enqueuedHttpProbeTargets;
    QSet<QNetworkReply *> m_httpProbeReplies;
    QQueue<QUrl> m_serverProbeCandidates;
    QNetworkReply *m_serverProbeReply = nullptr;
    QString m_serverProbeInput;
    QString m_tlsRetryInput;
    int m_inFlightHttpProbes = 0;
    bool m_active = false;
    bool m_foundAnyServer = false;
};

} // namespace JellyfinNative
