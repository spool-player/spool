#include "MovieGridModel.h"

#include <algorithm>
#include <utility>

namespace Spool {

namespace {

    double playbackProgress(const MovieItem& movie)
    {
        const qint64 resumeTicks = normalizedResumeTicks(movie.resumeTicks, movie.runtimeTicks);
        if (resumeTicks <= 0 || movie.runtimeTicks <= 0)
            return 0.0;
        return std::clamp(static_cast<double>(resumeTicks) / static_cast<double>(movie.runtimeTicks), 0.0, 1.0);
    }

    QString displayTitle(const MovieItem& movie)
    {
        if (movie.itemType == QStringLiteral("Episode") && !movie.seriesName.isEmpty())
            return movie.seriesName;
        return movie.title;
    }

}

MovieGridModel::MovieGridModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int MovieGridModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return isSparse() ? m_sparseCount : static_cast<int>(m_movies.size());
}

int MovieGridModel::slotOfRow(int row) const
{
    if (row < 0 || row >= rowCount())
        return -1;
    if (!isSparse())
        return row;
    const auto found = std::lower_bound(m_rows.begin(), m_rows.end(), row);
    return found != m_rows.end() && *found == row ? static_cast<int>(found - m_rows.begin()) : -1;
}

bool MovieGridModel::isLoaded(int row) const
{
    return slotOfRow(row) >= 0;
}

int MovieGridModel::count() const
{
    return rowCount();
}

QVariant MovieGridModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
        return {};

    const int slot = slotOfRow(index.row());
    if (slot < 0) {
        switch (role) {
        case ItemRole:
            return QVariant::fromValue(MovieItem {});
        case DisplayTitleRole:
        case DisplaySubtitleRole:
        case PlayActionLabelRole:
            return QString();
        case ProgressRole:
            return 0.0;
        case LoadedRole:
            return false;
        default:
            return {};
        }
    }
    const auto& movie = m_movies[static_cast<size_t>(slot)];
    switch (role) {
    case ItemRole:
        return QVariant::fromValue(movie);
    case DisplayTitleRole:
        return displayTitle(movie);
    case DisplaySubtitleRole:
        return itemDisplaySubtitle(movie);
    case ProgressRole:
        return playbackProgress(movie);
    case PlayActionLabelRole:
        return movie.playActionLabel();
    case LoadedRole:
        return true;
    default:
        return {};
    }
}

QHash<int, QByteArray> MovieGridModel::roleNames() const
{
    return {
        { ItemRole, "item" },
        { DisplayTitleRole, "displayTitle" },
        { DisplaySubtitleRole, "displaySubtitle" },
        { ProgressRole, "progress" },
        { PlayActionLabelRole, "playActionLabel" },
        { LoadedRole, "loaded" },
    };
}

MovieItem MovieGridModel::get(int index) const
{
    return movieAt(index);
}

void MovieGridModel::setMovies(std::vector<MovieItem> movies)
{
    if (isSparse()) {
        beginResetModel();
        m_sparseCount = -1;
        m_rows.clear();
        m_movies = std::move(movies);
        endResetModel();
        emit countChanged();
        return;
    }
    const int oldCount = rowCount();
    const int newCount = static_cast<int>(movies.size());

    // Refreshes usually deliver data identical to what is already shown.
    // Emitting only the rows that actually changed keeps live delegates
    // untouched instead of rebinding the whole grid mid-navigation.
    if (oldCount == newCount) {
        std::vector<std::pair<int, int>> changedRanges;
        int row = 0;
        while (row < newCount) {
            if (m_movies[static_cast<size_t>(row)] == movies[static_cast<size_t>(row)]) {
                ++row;
                continue;
            }
            const int first = row;
            while (row < newCount && !(m_movies[static_cast<size_t>(row)] == movies[static_cast<size_t>(row)]))
                ++row;
            changedRanges.emplace_back(first, row - 1);
        }
        if (changedRanges.empty())
            return;
        m_movies = std::move(movies);
        for (const auto& range : changedRanges)
            emit dataChanged(index(range.first, 0), index(range.second, 0));
        return;
    }

    // A grown list with an unchanged prefix is an append, not a reset;
    // resetting would destroy every delegate the user is looking at.
    if (newCount > oldCount && std::equal(m_movies.begin(), m_movies.end(), movies.begin())) {
        beginInsertRows({}, oldCount, newCount - 1);
        m_movies = std::move(movies);
        endInsertRows();
        emit countChanged();
        return;
    }

    beginResetModel();
    m_movies = std::move(movies);
    endResetModel();
    emit countChanged();
}

void MovieGridModel::appendMovies(const std::vector<MovieItem>& movies)
{
    if (movies.empty())
        return;
    if (isSparse()) {
        std::vector<MovieItem> rows(movies.begin(), movies.end());
        const int first = rowCount();
        beginInsertRows({}, first, first + static_cast<int>(rows.size()) - 1);
        m_sparseCount += static_cast<int>(rows.size());
        for (int offset = 0; offset < static_cast<int>(rows.size()); ++offset) {
            m_rows.push_back(first + offset);
            m_movies.push_back(std::move(rows[static_cast<size_t>(offset)]));
        }
        endInsertRows();
        emit countChanged();
        return;
    }

    const int first = rowCount();
    const int last = first + static_cast<int>(movies.size()) - 1;
    beginInsertRows({}, first, last);
    m_movies.insert(m_movies.end(), movies.begin(), movies.end());
    endInsertRows();
    emit countChanged();
}

void MovieGridModel::clear()
{
    const int oldCount = rowCount();
    beginResetModel();
    m_movies.clear();
    m_rows.clear();
    m_sparseCount = -1;
    endResetModel();
    if (oldCount != 0)
        emit countChanged();
}

MovieItem MovieGridModel::movieAt(int index) const
{
    const int slot = slotOfRow(index);
    if (slot < 0)
        return {};
    return m_movies[static_cast<size_t>(slot)];
}

void MovieGridModel::beginSparse(int count)
{
    const int oldCount = rowCount();
    beginResetModel();
    m_movies.clear();
    m_rows.clear();
    m_sparseCount = std::max(0, count);
    endResetModel();
    if (oldCount != m_sparseCount)
        emit countChanged();
}

void MovieGridModel::setRows(int first, std::vector<MovieItem> items)
{
    if (!isSparse() || items.empty() || first < 0 || first >= m_sparseCount)
        return;
    if (first + static_cast<int>(items.size()) > m_sparseCount)
        items.resize(static_cast<size_t>(m_sparseCount - first));
    const int last = first + static_cast<int>(items.size()) - 1;
    const auto begin = std::lower_bound(m_rows.begin(), m_rows.end(), first);
    const auto end = std::upper_bound(begin, m_rows.end(), last);
    const auto at = begin - m_rows.begin();
    m_movies.erase(m_movies.begin() + at, m_movies.begin() + (end - m_rows.begin()));
    m_rows.erase(begin, end);
    std::vector<int> rows(items.size());
    for (size_t offset = 0; offset < items.size(); ++offset)
        rows[offset] = first + static_cast<int>(offset);
    m_rows.insert(m_rows.begin() + at, rows.begin(), rows.end());
    m_movies.insert(
        m_movies.begin() + at, std::make_move_iterator(items.begin()), std::make_move_iterator(items.end()));
    emit dataChanged(index(first, 0), index(last, 0));
}

void MovieGridModel::unloadRows(int first, int last)
{
    if (!isSparse())
        return;
    first = std::max(0, first);
    last = std::min(m_sparseCount - 1, last);
    if (last < first)
        return;
    const auto begin = std::lower_bound(m_rows.begin(), m_rows.end(), first);
    const auto end = std::upper_bound(begin, m_rows.end(), last);
    if (begin == end)
        return;
    const auto at = begin - m_rows.begin();
    m_movies.erase(m_movies.begin() + at, m_movies.begin() + (end - m_rows.begin()));
    m_rows.erase(begin, end);
    emit dataChanged(index(first, 0), index(last, 0));
}

std::vector<MovieItem> MovieGridModel::loadedRange(int first, int last) const
{
    std::vector<MovieItem> result;
    first = std::max(0, first);
    last = std::min(rowCount() - 1, last);
    if (last < first)
        return result;
    if (!isSparse())
        return { m_movies.begin() + first, m_movies.begin() + last + 1 };
    const auto begin = std::lower_bound(m_rows.begin(), m_rows.end(), first);
    const auto end = std::upper_bound(begin, m_rows.end(), last);
    const auto at = begin - m_rows.begin();
    result.assign(m_movies.begin() + at, m_movies.begin() + (end - m_rows.begin()));
    return result;
}

const std::vector<MovieItem>& MovieGridModel::movies() const
{
    return m_movies;
}

bool MovieGridModel::updateResumeTicks(const QString& itemId, qint64 resumeTicks)
{
    if (itemId.isEmpty())
        return false;

    bool updated = false;
    for (int slot = 0; slot < loadedCount(); ++slot) {
        auto& movie = m_movies[static_cast<size_t>(slot)];
        const qint64 normalizedTicks = normalizedResumeTicks(resumeTicks, movie.runtimeTicks);
        if (movie.id != itemId || movie.resumeTicks == normalizedTicks)
            continue;

        movie.resumeTicks = normalizedTicks;
        const QModelIndex changed = index(rowOfSlot(slot), 0);
        emit dataChanged(changed, changed, { ItemRole, ProgressRole, PlayActionLabelRole });
        updated = true;
    }
    return updated;
}

bool MovieGridModel::updateFavorite(const QString& itemId, bool favorite)
{
    if (itemId.isEmpty())
        return false;

    bool updated = false;
    for (int slot = 0; slot < loadedCount(); ++slot) {
        auto& movie = m_movies[static_cast<size_t>(slot)];
        if (movie.id != itemId || movie.favorite == favorite)
            continue;

        movie.favorite = favorite;
        const QModelIndex changed = index(rowOfSlot(slot), 0);
        emit dataChanged(changed, changed, { ItemRole });
        updated = true;
    }
    return updated;
}

bool MovieGridModel::updatePlayed(const QString& itemId, bool played)
{
    if (itemId.isEmpty())
        return false;

    bool updated = false;
    for (int slot = 0; slot < loadedCount(); ++slot) {
        auto& movie = m_movies[static_cast<size_t>(slot)];
        const bool clearsResume = !played && movie.resumeTicks != 0;
        if (movie.id != itemId || (movie.played == played && !clearsResume))
            continue;

        movie.played = played;
        movie.resumeTicks = 0;
        const QModelIndex changed = index(rowOfSlot(slot), 0);
        emit dataChanged(changed, changed, { ItemRole, ProgressRole, PlayActionLabelRole });
        updated = true;
    }
    return updated;
}

bool MovieGridModel::removeUnresumable()
{
    if (isSparse())
        return false;
    bool removed = false;
    for (int row = rowCount() - 1; row >= 0; --row) {
        const MovieItem& movie = m_movies[static_cast<size_t>(row)];
        if (isMeaningfulResumePosition(movie.resumeTicks, movie.runtimeTicks))
            continue;

        beginRemoveRows({}, row, row);
        m_movies.erase(m_movies.begin() + row);
        endRemoveRows();
        removed = true;
    }
    if (removed)
        emit countChanged();
    return removed;
}

} // namespace Spool
