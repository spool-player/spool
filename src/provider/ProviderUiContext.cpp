#include "ProviderUiContext.h"

#include "ProviderRegistry.h"

#include <QJSEngine>
#include <QQmlEngine>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace Spool {
namespace {
    bool smallResult(const QVariant& value, int& values, qsizetype& bytes)
    {
        if (++values > 512)
            return false;
        if (value.metaType().id() == QMetaType::QVariantMap) {
            const auto map = value.toMap();
            for (auto it = map.cbegin(); it != map.cend(); ++it) {
                bytes += it.key().size() * sizeof(QChar);
                if (!smallResult(it.value(), values, bytes))
                    return false;
            }
        } else if (value.metaType().id() == QMetaType::QVariantList) {
            const auto list = value.toList();
            for (const auto& item : list) {
                if (!smallResult(item, values, bytes))
                    return false;
            }
        } else if (value.metaType().id() == QMetaType::QString) {
            bytes += value.toString().size() * sizeof(QChar);
        }
        return bytes <= 64 * 1024;
    }
}
ProviderListModel::ProviderListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_commitTimer.setSingleShot(true);
    connect(&m_commitTimer, &QTimer::timeout, this, &ProviderListModel::commitBatch);
}

int ProviderListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant ProviderListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    if (role == Qt::UserRole + 1)
        return m_rows.at(index.row());
    if (role == Qt::DisplayRole)
        return m_rows.at(index.row()).toMap().value(QStringLiteral("title"));
    return {};
}

QHash<int, QByteArray> ProviderListModel::roleNames() const
{
    return { { Qt::UserRole + 1, QByteArrayLiteral("record") }, { Qt::DisplayRole, QByteArrayLiteral("title") } };
}

bool ProviderListModel::enqueue(QVariantList rows, bool append)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (!m_pending.isEmpty() || rows.size() + (append ? m_rows.size() : 0) > 10000
        || std::any_of(rows.begin(), rows.end(),
            [](const QVariant& row) { return row.metaType().id() != QMetaType::QVariantMap; }))
        return false;
    if (!append) {
        beginResetModel();
        m_rows.clear();
        endResetModel();
    }
    m_pending = std::move(rows);
    m_position = 0;
    m_commitTimer.start(0);
    return true;
}

void ProviderListModel::commitBatch()
{
    Q_ASSERT(thread() == QThread::currentThread());
    const qsizetype count = std::min<qsizetype>(32, m_pending.size() - m_position);
    if (count > 0) {
        const int first = m_rows.size();
        beginInsertRows({}, first, first + count - 1);
        for (qsizetype i = 0; i < count; ++i)
            m_rows.append(std::move(m_pending[m_position++]));
        endInsertRows();
    }
    if (m_position < m_pending.size()) {
        m_commitTimer.start(0);
    } else {
        m_pending.clear();
        m_position = 0;
        emit committed();
    }
}

void ProviderListModel::cancelPending()
{
    m_commitTimer.stop();
    m_pending.clear();
    m_position = 0;
}

ProviderUiContext::ProviderUiContext(
    ProviderRegistry *registry, QString sourceId, QString moduleId, QString role, QUrl component)
    : QObject(registry)
    , m_registry(registry)
    , m_sourceId(std::move(sourceId))
    , m_moduleId(std::move(moduleId))
    , m_role(std::move(role))
    , m_component(std::move(component))
    , m_scope(QUuid::createUuid().toString(QUuid::WithoutBraces))
    , m_rows(this)
{
    QQmlEngine::setObjectOwnership(this, QQmlEngine::CppOwnership);
    connect(&m_rows, &ProviderListModel::committed, this, [this] {
        const quint64 request = std::exchange(m_listRequest, 0);
        settle(request, std::exchange(m_listResult, {}), true);
    });
}

ProviderUiContext::~ProviderUiContext()
{
    if (!m_closed)
        finish({}, true);
}

QJSValue ProviderUiContext::promise(Pending *pending)
{
    QJSEngine *engine = qjsEngine(this);
    if (!engine)
        return {};
    QJSValue bundle = engine->evaluate(
        QStringLiteral("(function() { let ok, no; const p = new Promise(function(a, b) { ok = a; no = b; });"
                       " return {promise: p, resolve: ok, reject: no}; })()"));
    pending->resolve = bundle.property(QStringLiteral("resolve"));
    pending->reject = bundle.property(QStringLiteral("reject"));
    return bundle.property(QStringLiteral("promise"));
}

QJSValue ProviderUiContext::request(const QString& operation, const QVariantMap& arguments)
{
    return begin(operation, arguments, false, false);
}

QJSValue ProviderUiContext::requestList(const QString& operation, const QVariantMap& arguments, bool append)
{
    return begin(operation, arguments, true, append);
}

QJSValue ProviderUiContext::begin(const QString& operation, const QVariantMap& arguments, bool list, bool append)
{
    Q_ASSERT(thread() == QThread::currentThread());
    Pending pending;
    QJSValue result = promise(&pending);
    if (m_closed || !m_registry || m_pending.size() >= 8 || (list && m_listRequest)) {
        pending.reject.call({ QJSValue(QStringLiteral("action_unavailable")) });
        return result;
    }
    const quint64 id = ++m_next;
    m_pending.insert(id, pending);
    if (list)
        m_listRequest = id;
    QPointer<ProviderUiContext> guard(this);
    m_registry->callSource(m_sourceId, operation, arguments, m_scope)
        .then(
            [guard, id, list, append](QVariantMap value) {
                if (!guard || guard->m_closed || !guard->m_pending.contains(id))
                    return;
                if (!list)
                    return guard->settle(id, value, true);
                const QVariant items = value.take(QStringLiteral("items"));
                if (items.metaType().id() != QMetaType::QVariantList || !guard->m_rows.enqueue(items.toList(), append))
                    return guard->settle(id, {}, false);
                guard->m_listResult = std::move(value);
            },
            [guard, id](const std::exception& error) {
                if (!guard)
                    return;
                if (guard->m_listRequest == id) {
                    guard->m_listRequest = 0;
                    guard->m_rows.cancelPending();
                }
                guard->settle(id, { { QStringLiteral("error"), QString::fromLatin1(error.what()) } }, false);
            });
    return result;
}

QJSValue ProviderUiContext::allowOrigin(const QString& url)
{
    Pending pending;
    QJSValue result = promise(&pending);
    if (m_closed || !m_registry || m_role != QStringLiteral("login")) {
        pending.reject.call({ QJSValue(QStringLiteral("origin_denied")) });
        return result;
    }
    const quint64 id = ++m_next;
    m_pending.insert(id, pending);
    QPointer<ProviderUiContext> guard(this);
    m_registry->allowSetupOrigin(m_sourceId, QUrl::fromUserInput(url))
        .then(
            [guard, id] {
                if (guard)
                    guard->settle(id, {}, true);
            },
            [guard, id](const std::exception&) {
                if (guard)
                    guard->settle(id, { { QStringLiteral("error"), QStringLiteral("origin_denied") } }, false);
            });
    return result;
}

void ProviderUiContext::settle(quint64 id, const QVariantMap& value, bool success)
{
    QJSEngine *engine = qjsEngine(this);
    if (!engine || !m_pending.contains(id))
        return;
    const Pending pending = m_pending.take(id);
    int values = 0;
    qsizetype bytes = 0;
    if (success && smallResult(value, values, bytes))
        pending.resolve.call({ engine->toScriptValue(value) });
    else
        // Errors are stable codes such as http_401 or invalid_credentials,
        // which the component maps to its own words.
        pending.reject.call(
            { QJSValue(value.value(QStringLiteral("error"), QStringLiteral("request_failed")).toString()) });
}

void ProviderUiContext::finish(const QVariantMap& result, bool cancelled)
{
    m_closed = true;
    if (m_registry)
        m_registry->cancelSourceScope(m_sourceId, m_scope);
    m_rows.cancelPending();
    m_listRequest = 0;
    for (const quint64 id : m_pending.keys())
        settle(id, {}, false);
    if (m_registry)
        m_registry->endContext(m_sourceId);
    emit closedChanged();
    emit finished(result, cancelled);
}

void ProviderUiContext::close()
{
    if (m_closed)
        return;
    finish({}, true);
    deleteLater();
}

void ProviderUiContext::complete(const QVariantMap& result)
{
    if (m_closed || !m_registry)
        return;
    if (m_role == QStringLiteral("login")) {
        if (m_registry->finishSetup(m_sourceId, result).isEmpty())
            return;
    } else if (m_role == QStringLiteral("settings") && result.contains(QStringLiteral("configuration"))) {
        m_registry->updateConfiguration(m_sourceId, result.value(QStringLiteral("configuration")).toMap());
        m_registry->restartAccount(m_sourceId);
    }
    finish(result, false);
    deleteLater();
}
} // namespace Spool
