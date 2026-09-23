#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

namespace Spool {

class RouterController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString route READ route NOTIFY routeChanged)
    Q_PROPERTY(QString previousRoute READ previousRoute NOTIFY routeChanged)
    Q_PROPERTY(QVariantMap args READ args NOTIFY routeChanged)
    Q_PROPERTY(QVariantList stack READ stack NOTIFY stackChanged)
    Q_PROPERTY(bool canPop READ canPop NOTIFY stackChanged)
    Q_PROPERTY(bool canForward READ canForward NOTIFY stackChanged)
    Q_PROPERTY(bool recoveryPending READ recoveryPending NOTIFY recoveryPendingChanged)

public:
    explicit RouterController(QObject *parent = nullptr);
    // `startRoute` is what the shell shows before the app has initialised:
    // the provider's login page when it has one, otherwise home.
    explicit RouterController(const QString& startRoute, QObject *parent = nullptr);

    QString route() const;
    QString previousRoute() const;
    QVariantMap args() const;
    QVariantList stack() const;
    bool canPop() const;
    bool canForward() const;
    bool recoveryPending() const;

    void beginSession(bool allowRecovery);
    void requestRecoveryOnNextLaunch(const QString& reason);
    void markCleanShutdown();

    Q_INVOKABLE void reset(const QString& route = QStringLiteral("home"), const QVariantMap& args = {});
    Q_INVOKABLE void push(const QString& route, const QVariantMap& args = {});
    Q_INVOKABLE void replace(const QString& route, const QVariantMap& args = {});
    Q_INVOKABLE bool pop(const QString& fallbackRoute = QStringLiteral("home"));
    Q_INVOKABLE bool forward();
    Q_INVOKABLE void checkpoint(const QVariantMap& args);
    Q_INVOKABLE void finishRecovery();

signals:
    void routeChanged();
    void stackChanged();
    void recoveryPendingChanged();

private:
    QVariantMap frame(const QString& route, const QVariantMap& args) const;
    void setFrame(const QString& route, const QVariantMap& args, const QString& previousRoute);
    void persistSnapshot() const;
    bool restoreSnapshot();

    QString m_route = QStringLiteral("login");
    QString m_previousRoute = QStringLiteral("home");
    QVariantMap m_args;
    QVariantList m_stack;
    QVariantList m_forwardStack;
    bool m_sessionStarted = false;
    bool m_recoveryPending = false;
    bool m_recoveryRequested = false;
};

} // namespace Spool
