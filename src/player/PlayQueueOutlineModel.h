#pragma once

#include <QHash>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QString>

#include <vector>

namespace Spool {
class PlayQueueController;

// The play queue with its automatically filled runs folded up.
//
// Starting an episode replaces the queue with the whole series, so the panel
// would otherwise open on several hundred rows of something nobody picked one
// by one. A run of consecutive episodes of a single series that arrived that
// way collapses; the run holding the playing row keeps the episode either
// side of it in view and folds the rest into a group before and a group
// after, and any other run folds to a single row.
//
// Rows the user queued themselves never fold. An episode you just added has
// to be visible where it landed, which is the whole point of adding it.
//
// The folded group is drawn by the run's own first row rather than by a
// synthetic one, so every visible row still maps to a real source row and
// selection, reordering and hit testing keep working unchanged.
class PlayQueueOutlineModel final : public QSortFilterProxyModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY outlineChanged)
    Q_PROPERTY(int currentRow READ currentRow NOTIFY outlineChanged)

public:
    // Far above the source model's roles, which start at Qt::UserRole + 1.
    enum Roles {
        OutlineKindRole = Qt::UserRole + 900,
        GroupCountRole,
        GroupLabelRole,
        GroupDetailRole,
        GroupExpandedRole,
        GroupFirstSourceRowRole,
        GroupLastSourceRowRole,
        // Set on every row of an opened group, so the panel can rule them
        // together without indenting them out of line with everything else.
        InGroupRole,
        GroupMemberIndexRole,
        UserQueuedRunStartRole,
    };

    // Shorter than this and folding costs more than it saves: the group row
    // would replace about as many rows as it hides.
    static constexpr int kMinimumRunToFold = 4;

    explicit PlayQueueOutlineModel(PlayQueueController *queue, QObject *parent = nullptr);

    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const
    {
        return rowCount();
    }
    int currentRow() const;

    Q_INVOKABLE int sourceRowAt(int row) const;
    Q_INVOKABLE int rowForSourceRow(int sourceRow) const;
    // Opens or closes the group drawn by this row. Returns false when the row
    // does not draw one.
    Q_INVOKABLE bool toggleGroup(int row);
    Q_INVOKABLE bool isGroup(int row) const;
    Q_INVOKABLE bool expandAll();
    // The span a folded group stands for, as { first, count }, so a drag can
    // carry the whole block or step over it in one press.
    Q_INVOKABLE QVariantMap groupSpanAt(int row) const;

signals:
    void outlineChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    struct Group {
        int first = 0;
        int last = 0;
        QString key;
        QString label;
        QString detail;
        bool expanded = false;
    };

    void markDirty();
    void rebuild() const;
    void ensureOutline() const
    {
        if (m_dirty)
            rebuild();
    }
    const Group *groupForSourceRow(int sourceRow) const;

    PlayQueueController *m_queue = nullptr;
    QSet<QString> m_expanded;

    mutable bool m_dirty = true;
    mutable std::vector<bool> m_hidden;
    // Index into m_groups for a row that draws a group header, else -1.
    mutable std::vector<int> m_headOf;
    // Index into m_groups for any row belonging to an opened group, else -1.
    mutable std::vector<int> m_memberOf;
    mutable std::vector<Group> m_groups;
};

} // namespace Spool
