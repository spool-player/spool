#pragma once

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QUrl>

class QQmlComponent;
class QQmlEngine;

namespace Spool {

// Retains compiled components, not instances: warming must not run provider
// bindings, Component.onCompleted handlers, timers or authentication flows.
class ProviderQmlCache final : public QObject {
    Q_OBJECT

public:
    explicit ProviderQmlCache(QQmlEngine *engine);
    // Selected, validated package roots, with trailing slashes. Enumeration is
    // deferred until launch; removed/replaced versions leave the warmup cache.
    void setPackages(const QList<QUrl>& roots);
    void start(int delayMs = 2500);
    void clear();
    int retainedCount() const;

signals:
    void finished();
    void componentReady(const QUrl& url);
    void componentFailed(const QUrl& url);

protected:
    bool event(QEvent *event) override;

private:
    void advance();
    void settle();

    QQmlEngine *m_engine;
    QTimer m_timer;
    QSet<QUrl> m_roots;
    QQueue<QUrl> m_pendingPackages;
    QQueue<QUrl> m_pending;
    QSet<QUrl> m_known;
    QHash<QUrl, QQmlComponent *> m_retained;
    QQmlComponent *m_loading = nullptr;
    bool m_started = false;
    bool m_advancePosted = false;
    bool m_stopped = false;
};

} // namespace Spool
