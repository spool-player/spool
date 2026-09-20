#include "ProviderQmlCache.h"

#include <QQmlComponent>
#include <QQmlEngine>
#include <QThread>

namespace JellyfinNative {

ProviderQmlCache::ProviderQmlCache(QQmlEngine *engine)
    : QObject(engine)
    , m_engine(engine)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &ProviderQmlCache::advance);
}

void ProviderQmlCache::addSources(const QList<QUrl>& sources)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (m_stopped)
        return;
    for (const QUrl& source : sources) {
        // Callers supply only sources from selected, trusted packages. Neither
        // remote URLs nor arbitrary recursive import discovery belong here.
        if ((source.scheme() != QStringLiteral("qrc") && !source.isLocalFile()) || m_known.contains(source))
            continue;
        m_known.insert(source);
        m_pending.enqueue(source);
    }
    if (m_started && !m_timer.isActive() && !m_loading && !m_pending.isEmpty())
        m_timer.start(100);
}

void ProviderQmlCache::start(int delayMs)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (m_started || m_stopped)
        return;
    m_started = true;
    m_timer.start(qMax(0, delayMs));
}

void ProviderQmlCache::advance()
{
    if (m_stopped || m_loading)
        return;
    if (m_pending.isEmpty()) {
        emit finished();
        return;
    }
    m_loading = new QQmlComponent(m_engine, this);
    connect(m_loading, &QQmlComponent::statusChanged, this, &ProviderQmlCache::settle);
    m_loading->loadUrl(m_pending.dequeue(), QQmlComponent::Asynchronous);
    // Cached components can complete without an asynchronous status change.
    settle();
}

void ProviderQmlCache::settle()
{
    if (!m_loading || m_loading->isLoading() || m_loading->isNull())
        return;
    QQmlComponent *component = m_loading;
    m_loading = nullptr;
    disconnect(component, nullptr, this, nullptr);
    const QUrl url = component->url();
    if (component->isReady()) {
        m_retained.insert(url, component);
        emit componentReady(url);
    } else {
        // Do not log provider error strings: they can contain private paths
        // and credential-bearing URLs. Normal activation reports the error.
        component->deleteLater();
        emit componentFailed(url);
    }
    // One compilation at a time, with an event-loop gap between components.
    // No timer remains running after the finite launch queue is exhausted.
    if (!m_stopped)
        m_timer.start(100);
}

void ProviderQmlCache::clear()
{
    Q_ASSERT(thread() == QThread::currentThread());
    m_stopped = true;
    m_timer.stop();
    m_pending.clear();
    m_known.clear();
    if (m_loading) {
        disconnect(m_loading, nullptr, this, nullptr);
        m_loading->deleteLater();
        m_loading = nullptr;
    }
    qDeleteAll(m_retained);
    m_retained.clear();
    m_engine->trimComponentCache();
}

int ProviderQmlCache::retainedCount() const
{
    return m_retained.size();
}

} // namespace JellyfinNative
