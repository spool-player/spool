#pragma once

#include "../media/MediaTypes.h"
#include <QCoroTask>
#include <QObject>
#include <QSet>
#include <QVariantList>

namespace Spool {

class SourceHub;

// An editor holds a consecutive native-order prefix. Occurrences, never media
// IDs, identify selection and mutations; duplicate media remain distinct rows.
class CollectionEditingController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString containerId READ containerId NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY changed)
    Q_PROPERTY(QString selectedEntryId READ selectedEntryId NOTIFY changed)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY changed)
    Q_PROPERTY(bool removable READ removable NOTIFY changed)
    Q_PROPERTY(bool movable READ movable NOTIFY changed)
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
public:
    explicit CollectionEditingController(SourceHub *sources, QObject *parent = nullptr);
    ~CollectionEditingController() override;
    QString containerId() const
    {
        return m_containerId;
    }
    QString title() const
    {
        return m_title;
    }
    QVariantList entries() const
    {
        return m_rows;
    }
    QString selectedEntryId() const
    {
        return m_selected;
    }
    int selectedIndex() const;
    bool busy() const
    {
        return m_busy;
    }
    bool hasMore() const
    {
        return !m_exhausted && !m_containerId.isEmpty();
    }
    bool removable() const
    {
        return m_removable;
    }
    bool movable() const
    {
        return m_movable;
    }
    QString problem() const
    {
        return m_problem;
    }
    Q_INVOKABLE void open(const QString& containerId, const QString& title);
    Q_INVOKABLE void close();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void requestNextPage();
    Q_INVOKABLE void selectEntry(const QString& entryId);
    Q_INVOKABLE void removeEntry(const QString& entryId);
    Q_INVOKABLE void moveEntry(const QString& entryId, int direction);
signals:
    void changed();

private:
    int indexOf(const QString& entryId) const;
    void beginLoad(bool reset);
    void mutate(const QString& entryId, int direction);
    QCoro::Task<void> load(quint64 generation, bool reset, int desiredCount);
    QCoro::Task<void> mutation(quint64 generation, QString entryId, int direction);
    void finish(quint64 generation);
    void fail(quint64 generation);
    void clearEntries();
    SourceHub *m_sources;
    QString m_containerId;
    QString m_accountId;
    QString m_title;
    QString m_selected;
    QString m_problem;
    std::vector<MovieItem> m_entries;
    QVariantList m_rows;
    std::optional<QString> m_cursor;
    QSet<QString> m_seenCursors;
    quint64 m_generation = 0;
    int m_pages = 0;
    bool m_busy = false;
    bool m_exhausted = true;
    bool m_removable = false;
    bool m_movable = false;
};

} // namespace Spool
