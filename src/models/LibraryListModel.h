#pragma once

#include "../media/MediaTypes.h"

#include <QAbstractListModel>

#include <vector>

namespace Spool {

class LibraryListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        CollectionTypeRole,
        ItemRole,
        ImageTagRole,
    };

    explicit LibraryListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int count() const;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE QVariantMap get(int index) const;

    void setLibraries(const std::vector<LibraryItem>& libraries);
    void clear();
    LibraryItem libraryAt(int index) const;
    const std::vector<LibraryItem>& libraries() const;

signals:
    void countChanged();

private:
    std::vector<LibraryItem> m_libraries;
};

} // namespace Spool
