#include "PlayQueueOutlineModel.h"

#include "PlayQueueController.h"

#include <QVariantMap>

#include <algorithm>

namespace Spool {
namespace {

    QString episodeCodeOf(const MovieItem& item)
    {
        return itemEpisodeCode(item);
    }

    // "S01E04", or the plain episode number when the server gave no season, or
    // the title when it gave neither.
    QString rowLabel(const MovieItem& item)
    {
        const QString code = episodeCodeOf(item);
        if (!code.isEmpty())
            return code;
        if (item.episodeNumber > 0)
            return QStringLiteral("Episode %1").arg(item.episodeNumber);
        return item.title;
    }

    QString rangeText(const MovieItem& first, const MovieItem& last)
    {
        const QString from = rowLabel(first);
        const QString to = rowLabel(last);
        if (from.isEmpty() || to.isEmpty())
            return {};
        return from == to ? from : QStringLiteral("%1 – %2").arg(from, to);
    }

    QString pluralEpisodes(int count)
    {
        return count == 1 ? QStringLiteral("1 episode") : QStringLiteral("%1 episodes").arg(count);
    }

} // namespace

PlayQueueOutlineModel::PlayQueueOutlineModel(PlayQueueController *queue, QObject *parent)
    : QSortFilterProxyModel(parent)
    , m_queue(queue)
{
    setSourceModel(queue);
    if (!queue)
        return;

    // Every signal that can change what folds, including the "about to"
    // halves: the base class asks filterAcceptsRow while handling the same
    // signals, and the cache has to already be invalid by then.
    const auto invalidate = [this] {
        markDirty();
        invalidateRowsFilter();
        emit outlineChanged();
    };
    connect(queue, &QAbstractItemModel::modelAboutToBeReset, this, [this] { markDirty(); });
    connect(queue, &QAbstractItemModel::rowsAboutToBeInserted, this, [this] { markDirty(); });
    connect(queue, &QAbstractItemModel::rowsAboutToBeRemoved, this, [this] { markDirty(); });
    connect(queue, &QAbstractItemModel::rowsAboutToBeMoved, this, [this] { markDirty(); });
    connect(queue, &QAbstractItemModel::modelReset, this, invalidate);
    connect(queue, &QAbstractItemModel::rowsInserted, this, invalidate);
    connect(queue, &QAbstractItemModel::rowsRemoved, this, invalidate);
    connect(queue, &QAbstractItemModel::rowsMoved, this, invalidate);
    connect(queue, &QAbstractItemModel::dataChanged, this, invalidate);
    // The playing row decides where the run holding it is split.
    connect(queue, &PlayQueueController::currentIndexChanged, this, invalidate);
    connect(queue, &PlayQueueController::queueChanged, this, invalidate);
}

void PlayQueueOutlineModel::markDirty()
{
    m_dirty = true;
}

void PlayQueueOutlineModel::rebuild() const
{
    m_dirty = false;
    m_groups.clear();
    const int rows = m_queue ? m_queue->rowCount() : 0;
    m_hidden.assign(static_cast<size_t>(rows), false);
    m_headOf.assign(static_cast<size_t>(rows), -1);
    m_memberOf.assign(static_cast<size_t>(rows), -1);
    if (rows <= 0)
        return;

    const int current = m_queue->currentIndex();
    // Runs of the show being watched read as "earlier"/"more" wherever they
    // sit; a different show names itself instead.
    const QString currentSeries = current >= 0 ? m_queue->itemAt(current).seriesId : QString();

    // A row can be folded when it arrived with a run rather than by hand, and
    // when it is an episode of a series -- the only thing the queue fills
    // itself with, and the only thing a group row can describe.
    const auto foldable = [this](int row) {
        const MovieItem item = m_queue->itemAt(row);
        return !m_queue->isUserQueued(row) && item.itemType == QStringLiteral("Episode") && !item.seriesId.isEmpty();
    };

    // Where a folded group sits relative to the playing row. "before" and
    // "after" are the two halves of the run being played; "whole" is a run
    // the cursor is nowhere near, which describes itself by name instead.
    enum class Fold { Before, After, Whole };

    const auto addGroup = [this](int first, int last, const QString& runKey, Fold fold) {
        if (first > last)
            return;
        const int span = last - first + 1;
        const MovieItem firstItem = m_queue->itemAt(first);
        const MovieItem lastItem = m_queue->itemAt(last);

        Group group;
        group.first = first;
        group.last = last;
        // Keyed to the run and the side rather than to the group's own first
        // row, so an opened "19 more episodes" stays open when the next
        // episode starts and moves the split along by one.
        const QString side = fold == Fold::Before ? QStringLiteral("before")
            : fold == Fold::After                 ? QStringLiteral("after")
                                                  : QStringLiteral("whole");
        group.key = QStringLiteral("%1|%2").arg(runKey, side);
        group.expanded = m_expanded.contains(group.key);

        const QString episodes = span == 1 ? QStringLiteral("episode") : QStringLiteral("episodes");
        switch (fold) {
        case Fold::Before:
            group.label = QStringLiteral("%1 earlier %2").arg(span).arg(episodes);
            group.detail = rangeText(firstItem, lastItem);
            break;
        case Fold::After:
            group.label = QStringLiteral("%1 more %2").arg(span).arg(episodes);
            group.detail = rangeText(firstItem, lastItem);
            break;
        case Fold::Whole: {
            const QString series = firstItem.seriesName.isEmpty() ? firstItem.title : firstItem.seriesName;
            const bool oneSeason = firstItem.seasonNumber > 0 && firstItem.seasonNumber == lastItem.seasonNumber;
            group.label = oneSeason ? QStringLiteral("%1 · Season %2").arg(series).arg(firstItem.seasonNumber) : series;
            group.detail = oneSeason
                ? pluralEpisodes(span)
                : QStringLiteral("%1 · %2").arg(pluralEpisodes(span), rangeText(firstItem, lastItem));
            break;
        }
        }

        const int index = static_cast<int>(m_groups.size());
        m_groups.push_back(group);
        m_headOf[static_cast<size_t>(first)] = index;
        for (int row = first; row <= last; ++row) {
            // Every row of the run knows its group whether the group is open
            // or shut, so a folded-away row can still be mapped to the row
            // that stands for it on screen.
            m_memberOf[static_cast<size_t>(row)] = index;
            m_hidden[static_cast<size_t>(row)] = !group.expanded && row != first;
        }
    };

    int row = 0;
    while (row < rows) {
        if (!foldable(row)) {
            ++row;
            continue;
        }
        const QString series = m_queue->itemAt(row).seriesId;
        int end = row;
        while (end + 1 < rows && foldable(end + 1) && m_queue->itemAt(end + 1).seriesId == series)
            ++end;

        const int span = end - row + 1;
        if (span < kMinimumRunToFold) {
            row = end + 1;
            continue;
        }

        const QString runKey = QStringLiteral("%1|%2").arg(series, m_queue->itemAt(row).id);
        if (current >= row && current <= end) {
            // Keep the episode either side of the playing one in view, and
            // fold the rest into a group before it and a group after it.
            addGroup(row, current - 2, runKey, Fold::Before);
            addGroup(current + 2, end, runKey, Fold::After);
        } else if (!currentSeries.isEmpty() && series == currentSeries) {
            // Still the show being watched, just cut off from the playing row
            // by something queued between them. "More episodes" is what it
            // is, even though the run itself does not hold the cursor.
            addGroup(row, end, runKey, current > end ? Fold::Before : Fold::After);
        } else {
            addGroup(row, end, runKey, Fold::Whole);
        }
        row = end + 1;
    }
}

const PlayQueueOutlineModel::Group *PlayQueueOutlineModel::groupForSourceRow(int sourceRow) const
{
    ensureOutline();
    if (sourceRow < 0 || sourceRow >= static_cast<int>(m_headOf.size()))
        return nullptr;
    const int head = m_headOf[static_cast<size_t>(sourceRow)];
    if (head >= 0)
        return &m_groups[static_cast<size_t>(head)];
    const int member = m_memberOf[static_cast<size_t>(sourceRow)];
    return member >= 0 ? &m_groups[static_cast<size_t>(member)] : nullptr;
}

bool PlayQueueOutlineModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    Q_UNUSED(sourceParent)
    ensureOutline();
    if (sourceRow < 0 || sourceRow >= static_cast<int>(m_hidden.size()))
        return true;
    return !m_hidden[static_cast<size_t>(sourceRow)];
}

QVariant PlayQueueOutlineModel::data(const QModelIndex& index, int role) const
{
    if (role < OutlineKindRole)
        return QSortFilterProxyModel::data(index, role);
    if (!index.isValid())
        return {};

    ensureOutline();
    const int sourceRow = mapToSource(index).row();
    const int head = sourceRow >= 0 && sourceRow < static_cast<int>(m_headOf.size())
        ? m_headOf[static_cast<size_t>(sourceRow)]
        : -1;
    const Group *group = groupForSourceRow(sourceRow);

    switch (role) {
    case OutlineKindRole:
        // A closed group's head row stands for the whole run and draws as
        // one. An open group's head row draws the band and its own content
        // underneath, so it is both.
        return head >= 0
            ? (m_groups[static_cast<size_t>(head)].expanded ? QStringLiteral("groupOpen") : QStringLiteral("group"))
            : QStringLiteral("item");
    case GroupCountRole:
        return group ? group->last - group->first + 1 : 0;
    case GroupLabelRole:
        return group ? group->label : QString();
    case GroupDetailRole:
        return group ? group->detail : QString();
    case GroupExpandedRole:
        return group ? group->expanded : false;
    case GroupFirstSourceRowRole:
        return group ? group->first : -1;
    case GroupLastSourceRowRole:
        return group ? group->last : -1;
    case InGroupRole:
        // Only an opened group rules its members together; a shut one has no
        // members on screen to rule.
        return group != nullptr && group->expanded;
    case GroupMemberIndexRole:
        return group ? sourceRow - group->first : -1;
    case UserQueuedRunStartRole:
        // True on the first row the user queued after a run of automatic
        // ones, which is where the panel draws its divider.
        return m_queue && m_queue->isUserQueued(sourceRow) && (sourceRow == 0 || !m_queue->isUserQueued(sourceRow - 1));
    default:
        return {};
    }
}

QHash<int, QByteArray> PlayQueueOutlineModel::roleNames() const
{
    QHash<int, QByteArray> names = QSortFilterProxyModel::roleNames();
    names.insert(OutlineKindRole, QByteArrayLiteral("outlineKind"));
    names.insert(GroupCountRole, QByteArrayLiteral("groupCount"));
    names.insert(GroupLabelRole, QByteArrayLiteral("groupLabel"));
    names.insert(GroupDetailRole, QByteArrayLiteral("groupDetail"));
    names.insert(GroupExpandedRole, QByteArrayLiteral("groupExpanded"));
    names.insert(GroupFirstSourceRowRole, QByteArrayLiteral("groupFirstSourceRow"));
    names.insert(GroupLastSourceRowRole, QByteArrayLiteral("groupLastSourceRow"));
    names.insert(InGroupRole, QByteArrayLiteral("inGroup"));
    names.insert(GroupMemberIndexRole, QByteArrayLiteral("groupMemberIndex"));
    names.insert(UserQueuedRunStartRole, QByteArrayLiteral("userQueuedRunStart"));
    return names;
}

int PlayQueueOutlineModel::currentRow() const
{
    return m_queue ? rowForSourceRow(m_queue->currentIndex()) : -1;
}

int PlayQueueOutlineModel::sourceRowAt(int row) const
{
    const QModelIndex proxyIndex = index(row, 0);
    return proxyIndex.isValid() ? mapToSource(proxyIndex).row() : -1;
}

int PlayQueueOutlineModel::rowForSourceRow(int sourceRow) const
{
    if (!m_queue || sourceRow < 0 || sourceRow >= m_queue->rowCount())
        return -1;
    ensureOutline();
    // A hidden row is represented on screen by the group that swallowed it.
    int visibleRow = sourceRow;
    if (sourceRow < static_cast<int>(m_hidden.size()) && m_hidden[static_cast<size_t>(sourceRow)]) {
        const Group *group = groupForSourceRow(sourceRow);
        visibleRow = group ? group->first : sourceRow;
    }
    const QModelIndex mapped = mapFromSource(m_queue->index(visibleRow, 0));
    return mapped.isValid() ? mapped.row() : -1;
}

bool PlayQueueOutlineModel::isGroup(int row) const
{
    const int sourceRow = sourceRowAt(row);
    ensureOutline();
    return sourceRow >= 0 && sourceRow < static_cast<int>(m_headOf.size())
        && m_headOf[static_cast<size_t>(sourceRow)] >= 0;
}

QVariantMap PlayQueueOutlineModel::groupSpanAt(int row) const
{
    const int sourceRow = sourceRowAt(row);
    const Group *group = groupForSourceRow(sourceRow);
    if (!group)
        return { { QStringLiteral("first"), sourceRow }, { QStringLiteral("count"), sourceRow >= 0 ? 1 : 0 } };
    return { { QStringLiteral("first"), group->first }, { QStringLiteral("count"), group->last - group->first + 1 } };
}

bool PlayQueueOutlineModel::toggleGroup(int row)
{
    const int sourceRow = sourceRowAt(row);
    ensureOutline();
    if (sourceRow < 0 || sourceRow >= static_cast<int>(m_headOf.size()))
        return false;
    const int head = m_headOf[static_cast<size_t>(sourceRow)];
    if (head < 0)
        return false;

    const QString key = m_groups[static_cast<size_t>(head)].key;
    if (m_expanded.contains(key))
        m_expanded.remove(key);
    else
        m_expanded.insert(key);

    markDirty();
    invalidateRowsFilter();
    emit outlineChanged();
    return true;
}

} // namespace Spool
