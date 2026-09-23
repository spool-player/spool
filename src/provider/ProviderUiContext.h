#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QJSValue>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

namespace Spool {
class ProviderRegistry;

// Rows from a provider list, already native-owned. Role reads never enter a
// provider engine; insertion is bounded per GUI event-loop turn.
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

// What a mounted provider component (login, settings, picker) is given: its
// own source to call, nothing else. It settles once, with complete() or
// close(), and closing cancels whatever it still has in flight.
class ProviderUiContext final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString sourceId READ sourceId CONSTANT)
    Q_PROPERTY(QString moduleId READ moduleId CONSTANT)
    Q_PROPERTY(QString role READ role CONSTANT)
    Q_PROPERTY(QUrl component READ component CONSTANT)
    Q_PROPERTY(QVariantMap arguments READ arguments CONSTANT)
    Q_PROPERTY(bool closed READ closed NOTIFY closedChanged)
    Q_PROPERTY(Spool::ProviderListModel *rows READ rows CONSTANT)

public:
    ProviderUiContext(ProviderRegistry *registry, QString sourceId, QString moduleId, QString role, QUrl component);
    ~ProviderUiContext() override;

    QString sourceId() const
    {
        return m_sourceId;
    }
    QString moduleId() const
    {
        return m_moduleId;
    }
    QString role() const
    {
        return m_role;
    }
    QUrl component() const
    {
        return m_component;
    }
    QVariantMap arguments() const
    {
        return m_arguments;
    }
    void setArguments(QVariantMap arguments)
    {
        m_arguments = std::move(arguments);
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
    // Moves `items` into `rows`; the promise resolves with the rest.
    Q_INVOKABLE QJSValue requestList(const QString& operation, const QVariantMap& arguments = {}, bool append = false);
    // Setup only: lets this source reach the server the viewer typed or
    // picked. Resolves once calls to it can be made.
    Q_INVOKABLE QJSValue allowOrigin(const QString& url);
    // login: {account, label, detail?, group?, configuration}
    // settings: {configuration} to save and reconnect with, or {}
    // picker: the arguments to resolve again with
    Q_INVOKABLE void complete(const QVariantMap& result);
    Q_INVOKABLE void close();

signals:
    void closedChanged();
    void finished(const QVariantMap& result, bool cancelled);

private:
    struct Pending {
        QJSValue resolve;
        QJSValue reject;
    };
    QJSValue begin(const QString& operation, const QVariantMap& arguments, bool list, bool append);
    QJSValue promise(Pending *pending);
    void settle(quint64 id, const QVariantMap& value, bool success);
    void finish(const QVariantMap& result, bool cancelled);

    QPointer<ProviderRegistry> m_registry;
    QString m_sourceId;
    QString m_moduleId;
    QString m_role;
    QUrl m_component;
    QString m_scope;
    QVariantMap m_arguments;
    bool m_closed = false;
    quint64 m_next = 0;
    QHash<quint64, Pending> m_pending;
    ProviderListModel m_rows;
    quint64 m_listRequest = 0;
    QVariantMap m_listResult;
};
} // namespace Spool
