#include "ProviderUiContext.h"

#include "ProviderRegistry.h"

#include <QQmlEngine>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace JellyfinNative {
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

ProviderUiContext::ProviderUiContext(ProviderRegistry *registry, QString sourceId, QQmlEngine *engine)
    : QObject(engine)
    , m_registry(registry)
    , m_engine(engine)
    , m_sourceId(std::move(sourceId))
    , m_scope(QUuid::createUuid().toString(QUuid::WithoutBraces))
    , m_rows(this)
{
    QQmlEngine::setObjectOwnership(this, QQmlEngine::CppOwnership);
    connect(&m_rows, &ProviderListModel::committed, this, [this] {
        const quint64 request = std::exchange(m_listRequest, 0);
        auto result = std::exchange(m_listResult, {});
        settle(request, result, true);
    });
    connect(registry, &ProviderRegistry::sourceRemoved, this, [this](const QString& id) {
        if (id == m_sourceId)
            close();
    });
    connect(registry, &QObject::destroyed, this, &ProviderUiContext::close);
    connect(registry, &ProviderRegistry::configuredSourcesChanged, this, [this] {
        if (!m_registry)
            return;
        const auto sources = m_registry->configuredSources();
        const bool available = std::any_of(sources.begin(), sources.end(), [this](const QVariant& value) {
            const auto source = value.toMap();
            return source.value(QStringLiteral("sourceId")) == m_sourceId
                && source.value(QStringLiteral("available")).toBool();
        });
        if (!available)
            close();
    });
}

ProviderUiContext::~ProviderUiContext()
{
    close();
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
    if (!m_engine)
        return {};
    QJSValue bundle = m_engine->evaluate(QStringLiteral(R"JS(
        (function() {
            let resolve, reject;
            const promise = new Promise(function(ok, fail) { resolve = ok; reject = fail; });
            return {promise: promise, resolve: resolve, reject: reject};
        })()
    )JS"));
    QJSValue promise = bundle.property(QStringLiteral("promise"));
    if (m_closed || !m_registry || m_pending.size() >= 8 || (list && m_listRequest)) {
        bundle.property(QStringLiteral("reject")).call({ QJSValue(QStringLiteral("action_unavailable")) });
        return promise;
    }
    const quint64 id = ++m_next;
    m_pending.insert(id, { bundle.property(QStringLiteral("resolve")), bundle.property(QStringLiteral("reject")) });
    if (list)
        m_listRequest = id;
    QPointer<ProviderUiContext> guard(this);
    m_registry->callSource(m_sourceId, operation, arguments, m_scope)
        .then(
            [guard, id, list, append](QVariantMap result) {
                if (guard)
                    guard->resolved(id, std::move(result), list, append);
            },
            [guard, id](const std::exception&) {
                if (guard)
                    guard->rejected(id);
            });
    return promise;
}

void ProviderUiContext::resolved(quint64 id, QVariantMap value, bool list, bool append)
{
    if (m_closed || !m_pending.contains(id))
        return;
    if (list) {
        const QVariant items = value.take(QStringLiteral("items"));
        if (items.metaType().id() != QMetaType::QVariantList || !m_rows.enqueue(items.toList(), append)) {
            rejected(id);
            return;
        }
        m_listResult = std::move(value);
    } else {
        settle(id, value, true);
    }
}

void ProviderUiContext::rejected(quint64 id)
{
    if (m_listRequest == id) {
        m_listRequest = 0;
        m_listResult.clear();
        m_rows.cancelPending();
    }
    settle(id, {}, false);
}

void ProviderUiContext::settle(quint64 id, const QVariantMap& value, bool success)
{
    if (!m_engine || !m_pending.contains(id))
        return;
    const Pending pending = m_pending.take(id);
    int values = 0;
    qsizetype bytes = 0;
    success = success && smallResult(value, values, bytes);
    if (success)
        pending.resolve.call({ m_engine->toScriptValue(value) });
    else
        pending.reject.call({ QJSValue(QStringLiteral("action_cancelled_or_failed")) });
}

void ProviderUiContext::cancelRequests()
{
    if (m_registry)
        m_registry->cancelSourceScope(m_sourceId, m_scope);
    m_rows.cancelPending();
    m_listRequest = 0;
    m_listResult.clear();
    const auto ids = m_pending.keys();
    for (quint64 id : ids)
        settle(id, {}, false);
}

void ProviderUiContext::close()
{
    if (m_closed)
        return;
    m_closed = true;
    cancelRequests();
    emit closedChanged();
    emit finished({}, true);
}

void ProviderUiContext::complete(const QVariantMap& result)
{
    if (m_closed)
        return;
    m_closed = true;
    cancelRequests();
    emit closedChanged();
    QVariantMap scopedResult = result;
    scopedResult.insert(QStringLiteral("sourceId"), m_sourceId);
    emit finished(scopedResult, false);
}
} // namespace JellyfinNative
