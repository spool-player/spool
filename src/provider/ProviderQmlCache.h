#pragma once

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QUrl>

class QQmlComponent;
class QQmlEngine;

namespace JellyfinNative {

// Retains compiled components, not instances: warming must not run provider
// bindings, Component.onCompleted handlers, timers or authentication flows.
class ProviderQmlCache final : public QObject {
    Q_OBJECT

public:
    explicit ProviderQmlCache(QQmlEngine *engine);
    void addSources(const QList<QUrl>& sources);
    void start(int delayMs = 2500);
    void clear();
    int retainedCount() const;

signals:
    void finished();
    void componentReady(const QUrl& url);
    void componentFailed(const QUrl& url);

private:
    void advance();
    void settle();

    QQmlEngine *m_engine;
    QTimer m_timer;
    QQueue<QUrl> m_pending;
    QSet<QUrl> m_known;
    QHash<QUrl, QQmlComponent *> m_retained;
    QQmlComponent *m_loading = nullptr;
    bool m_started = false;
    bool m_stopped = false;
};

} // namespace JellyfinNative
