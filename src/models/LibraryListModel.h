#pragma once

#include "../media/MediaTypes.h"

#include <QAbstractListModel>
#include <QStringList>
#include <QVariantList>

#include <vector>

namespace Spool {

class LibraryListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QVariantList hiddenLibraries READ hiddenLibraries NOTIFY hiddenLibrariesChanged)

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
    Q_INVOKABLE bool moveLibrary(int from, int to);
    Q_INVOKABLE bool hideLibrary(const QString& libraryId);
    Q_INVOKABLE bool showLibrary(const QString& libraryId);
    Q_INVOKABLE bool isHidden(const QString& libraryId) const;
    QVariantList hiddenLibraries() const;

    void setLibraries(const std::vector<LibraryItem>& libraries);
    void clear();
    LibraryItem libraryAt(int index) const;
    LibraryItem libraryById(const QString& libraryId) const;
    // Full catalog, including hidden libraries: refreshes must not discard data.
    const std::vector<LibraryItem>& libraries() const;

signals:
    void countChanged();
    void hiddenLibrariesChanged();

private:
    std::vector<LibraryItem> m_libraries;
    std::vector<size_t> m_visibleIndices;
    QStringList m_hiddenIds;
    void rebuildVisibleIndices();
    void saveOrder() const;
};

} // namespace Spool
