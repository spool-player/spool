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
    const std::vector<MovieItem>& movies() const;
    bool updateResumeTicks(const QString& itemId, qint64 resumeTicks);
    bool updateFavorite(const QString& itemId, bool favorite);
    bool updatePlayed(const QString& itemId, bool played);
    bool removeUnresumable();

signals:
    void countChanged();

private:
    std::vector<MovieItem> m_movies;
};

} // namespace Spool
