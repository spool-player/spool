#pragma once

#include "../api/JellyfinSession.h"
#include "../media/MediaTypes.h"

#include <QObject>
#include <QTimer>
#include <QtTypes>

namespace JellyfinNative {

class JellyfinApiFacade;

class QuickConnectController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString code READ code NOTIFY changed)
    // Who the flow is waiting on: "user" until the code is approved elsewhere,
    // "server" while the token is exchanged, empty when nothing is running.
    Q_PROPERTY(QString phase READ phase NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY changed)

public:
    explicit QuickConnectController(JellyfinApiFacade *api, QObject *parent = nullptr);

    QString code() const;
    QString phase() const;
    bool active() const;

    Q_INVOKABLE void start(const QString& serverUrl);
    Q_INVOKABLE void cancel();

signals:
    void changed();
    void busyChanged(bool busy, const QString& text);
    void errorOccurred(const QString& message);
    void authenticated(const JellyfinNative::AuthSession& session);

private:
    void poll();

    JellyfinApiFacade *m_api = nullptr;
    QTimer m_pollTimer;
    QString m_code;
    QString m_phase;
    QString m_secret;
    int m_pollAttempts = 0;
    int m_pollErrors = 0;
    quint64 m_generation = 0;
};

} // namespace JellyfinNative
