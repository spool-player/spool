#include "ProviderQmlCache.h"

#include <QCoreApplication>
#include <QDirIterator>
#include <QEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QThread>

namespace Spool {

namespace {
    const auto advanceEventType = static_cast<QEvent::Type>(QEvent::registerEventType());
}

ProviderQmlCache::ProviderQmlCache(QQmlEngine *engine)
    : QObject(engine)
    , m_engine(engine)
{
    Q_ASSERT(engine->thread() == QThread::currentThread());
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (m_stopped || m_advancePosted)
            return;
        m_advancePosted = true;
        QCoreApplication::postEvent(this, new QEvent(advanceEventType), Qt::LowEventPriority);
    });
}

void ProviderQmlCache::setPackages(const QList<QUrl>& roots)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (m_stopped)
        return;
    QSet<QUrl> selected;
    for (const QUrl& root : roots) {
        // Only selected, validated packages, never remote import discovery.
        if ((root.scheme() == QStringLiteral("qrc") || root.isLocalFile()) && root.path().endsWith(QLatin1Char('/')))
            selected.insert(root);
    }
    const auto obsolete = [&selected](const QUrl& url) {
        for (const QUrl& root : selected) {
            if (root.isParentOf(url))
                return false;
        }
        return true;
    };
    m_pendingPackages.removeIf([&selected](const QUrl& root) { return !selected.contains(root); });
    m_pending.removeIf(obsolete);
    m_known.removeIf(obsolete);
    if (m_loading && obsolete(m_loading->url())) {
        disconnect(m_loading, nullptr, this, nullptr);
        m_loading->deleteLater();
        m_loading = nullptr;
    }
    for (auto it = m_retained.begin(); it != m_retained.end();) {
        if (obsolete(it.key())) {
            delete it.value();
            it = m_retained.erase(it);
        } else {
            ++it;
        }
    }
    for (const QUrl& root : selected) {
        if (!m_roots.contains(root))
            m_pendingPackages.enqueue(root);
    }
    m_roots = std::move(selected);
    if (m_started && !m_timer.isActive() && !m_advancePosted && !m_loading
        && (!m_pending.isEmpty() || !m_pendingPackages.isEmpty()))
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

bool ProviderQmlCache::event(QEvent *event)
{
    if (event->type() != advanceEventType)
        return QObject::event(event);
    m_advancePosted = false;
    advance();
    return true;
}

void ProviderQmlCache::advance()
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (m_stopped || m_loading)
        return;
    if (m_pending.isEmpty() && !m_pendingPackages.isEmpty()) {
        const QUrl root = m_pendingPackages.dequeue();
        const QString path = root.isLocalFile() ? root.toLocalFile() : QLatin1Char(':') + root.path();
        // Validated packages contain at most 512 files. Enumerate one package
        // per low-priority turn, including helpers loaded dynamically by QML.
        QDirIterator files(path, { QStringLiteral("*.qml") }, QDir::Files | QDir::Hidden | QDir::NoSymLinks,
            QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString file = files.next();
            const QUrl url = root.isLocalFile() ? QUrl::fromLocalFile(file) : QUrl(QStringLiteral("qrc") + file);
            if (!m_known.contains(url)) {
                m_known.insert(url);
                m_pending.enqueue(url);
            }
        }
        m_timer.start(100);
        return;
    }
    if (m_pending.isEmpty()) {
        emit finished();
        return;
    }
    // QQmlComponent and the engine stay on their owning thread. Asynchronous
    // loading lets Qt's existing type loader compile without a second engine.
    m_loading = new QQmlComponent(m_engine, this);
    connect(m_loading, &QQmlComponent::statusChanged, this, &ProviderQmlCache::settle);
    m_loading->loadUrl(m_pending.dequeue(), QQmlComponent::Asynchronous);
    // Cached components can complete without an asynchronous status change.
    settle();
}

void ProviderQmlCache::settle()
{
    Q_ASSERT(thread() == QThread::currentThread());
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
    QCoreApplication::removePostedEvents(this, advanceEventType);
    m_advancePosted = false;
    m_pending.clear();
    m_known.clear();
    m_roots.clear();
    m_pendingPackages.clear();
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

} // namespace Spool
