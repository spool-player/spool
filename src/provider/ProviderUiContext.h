#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QJSValue>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>

class QQmlEngine;

namespace JellyfinNative {
class ProviderRegistry;

// Values are already native-owned when they arrive here. Role reads never
// enter a provider engine; insertion is bounded per GUI event-loop turn.
class ProviderListModel final : public QAbstractListModel {
    Q_OBJECT
public:
    explicit ProviderListModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    bool enqueue(QVariantList rows, bool append);
    void cancelPending();

signals:
    void committed();

private:
    void commitBatch();
    QVariantList m_rows;
    QVariantList m_pending;
    qsizetype m_position = 0;
    QTimer m_commitTimer;
};

// A single mounted action, bound by native code to one persistent source.
// QML sees this facade, never a worker QObject or the global registry.
class ProviderUiContext final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString sourceId READ sourceId CONSTANT)
    Q_PROPERTY(bool closed READ closed NOTIFY closedChanged)
    Q_PROPERTY(JellyfinNative::ProviderListModel *rows READ rows CONSTANT)
public:
    ProviderUiContext(ProviderRegistry *registry, QString sourceId, QQmlEngine *engine);
    ~ProviderUiContext() override;
    QString sourceId() const
    {
        return m_sourceId;
    }
    bool closed() const
    {
        return m_closed;
    }
    ProviderListModel *rows()
    {
        return &m_rows;
    }
    Q_INVOKABLE QJSValue request(const QString& operation, const QVariantMap& arguments = {});
    Q_INVOKABLE QJSValue requestList(const QString& operation, const QVariantMap& arguments = {}, bool append = false);
    Q_INVOKABLE void complete(const QVariantMap& result);
    Q_INVOKABLE void close();

signals:
    void closedChanged();
    void finished(const QVariantMap& result, bool cancelled);

private:
    QJSValue begin(const QString& operation, const QVariantMap& arguments, bool list, bool append);
    void resolved(quint64 id, QVariantMap value, bool list, bool append);
    void rejected(quint64 id);
    void settle(quint64 id, const QVariantMap& value, bool success);
    void cancelRequests();

    struct Pending {
        QJSValue resolve;
        QJSValue reject;
    };
    QPointer<ProviderRegistry> m_registry;
    QPointer<QQmlEngine> m_engine;
    QString m_sourceId;
    QString m_scope;
    bool m_closed = false;
    quint64 m_next = 0;
    QHash<quint64, Pending> m_pending;
    ProviderListModel m_rows;
    quint64 m_listRequest = 0;
    QVariantMap m_listResult;
};
} // namespace JellyfinNative
