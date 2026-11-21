#pragma once

#include "../media/MediaTypes.h"

#include <QAbstractListModel>

#include <vector>

namespace Spool {

class MovieGridModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        ItemRole = Qt::UserRole + 1,
        DisplayTitleRole,
        DisplaySubtitleRole,
        ProgressRole,
        PlayActionLabelRole,
        LoadedRole,
    };

    explicit MovieGridModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int count() const;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    Q_INVOKABLE MovieItem get(int index) const;

    void setMovies(std::vector<MovieItem> movies);
    void appendMovies(const std::vector<MovieItem>& movies);
    void clear();
    MovieItem movieAt(int index) const;
    // Every row holding an item, in row order. In sparse mode that is only
    // the loaded rows, so an index into it is not a row number there.
    const std::vector<MovieItem>& movies() const;

    // Sparse mode: the model has `count` rows but holds only the ones that
    // have been loaded; the rest are placeholders (loaded == false) until a
    // block of rows arrives. Ends with setMovies() or clear().
    void beginSparse(int count);
    bool isSparse() const
    {
        return m_sparseCount >= 0;
    }
    bool isLoaded(int row) const;
    // Fills rows [first, first + items.size()), replacing what was there.
    void setRows(int first, std::vector<MovieItem> items);
    // Returns rows [first, last] to placeholders, releasing their items.
    void unloadRows(int first, int last);
    int loadedCount() const
    {
        return static_cast<int>(m_movies.size());
    }
    // The loaded items whose rows fall in [first, last], in row order.
    std::vector<MovieItem> loadedRange(int first, int last) const;
    // Row of the loaded item at `slot` in movies().
    int rowOfSlot(int slot) const
    {
        return isSparse() ? m_rows[static_cast<size_t>(slot)] : slot;
    }
    bool updateResumeTicks(const QString& itemId, qint64 resumeTicks);
    bool updateFavorite(const QString& itemId, bool favorite);
    bool updatePlayed(const QString& itemId, bool played);
    bool removeUnresumable();

signals:
    void countChanged();

private:
    // movies() position of a row, or -1 for a placeholder.
    int slotOfRow(int row) const;

    std::vector<MovieItem> m_movies;
    // Sparse mode only: the row of each m_movies entry, ascending.
    std::vector<int> m_rows;
    int m_sparseCount = -1;
};

} // namespace Spool
