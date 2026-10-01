#pragma once

#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QVariantMap>

#include <functional>
#include <memory>

namespace Spool {

class LanProbe;
struct LanProbeSession {
    std::function<QList<QHostAddress>()> targets;
    QList<QHostAddress> snapshot;
    QString cursor;
    QString path;
    int port = 0;
    qsizetype next = 0;
    QPointer<LanProbe> active;
};

// Pure ranking/target selection, separated from I/O for deterministic fixtures.
struct LanNetwork {
    QString name;
    QNetworkInterface::InterfaceFlags flags;
    QHostAddress address;
    int prefixLength = 0;
};
QList<QHostAddress> lanProbeTargets(QList<LanNetwork> networks);
QList<QHostAddress> localLanProbeTargets();

class LanProbe final : public QObject {
public:
    using Completion = std::function<void(QString, QVariantMap)>;
    LanProbe(std::shared_ptr<LanProbeSession> session, Completion completion, QObject *parent);
    ~LanProbe() override;
    void start(const QVariantMap& options);
    void cancel();

private:
    void pump();
    void finish(QString error = {});
    void abortReplies();
    std::shared_ptr<LanProbeSession> m_session;
    Completion m_completion;
    QNetworkAccessManager m_network;
    QTimer m_deadline;
    QElapsedTimer m_elapsed;
    QSet<QNetworkReply *> m_replies;
    QVariantList m_responses;
    qsizetype m_end = 0;
    bool m_finished = false;
};

} // namespace Spool
