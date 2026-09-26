#include "LibraryListModel.h"

namespace Spool {

LibraryListModel::LibraryListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int LibraryListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_libraries.size());
}

int LibraryListModel::count() const
{
    return rowCount();
}

QVariant LibraryListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
        return {};

    const auto& library = m_libraries[static_cast<size_t>(index.row())];
    switch (role) {
    case IdRole:
        return library.id;
    case NameRole:
        return library.name;
    case CollectionTypeRole:
        return library.collectionType;
    case ItemRole:
        return QVariant::fromValue(library);
    case ImageTagRole:
        return library.imageTag;
    default:
        return {};
    }
}

QHash<int, QByteArray> LibraryListModel::roleNames() const
{
    return {
        { IdRole, "libraryId" },
        { NameRole, "name" },
        { CollectionTypeRole, "collectionType" },
        { ItemRole, "item" },
        { ImageTagRole, "imageTag" },
    };
}

QVariantMap LibraryListModel::get(int index) const
{
    if (index < 0 || index >= rowCount())
        return {};

    const auto& library = m_libraries[static_cast<size_t>(index)];
    return {
        { QStringLiteral("libraryId"), library.id },
        { QStringLiteral("name"), library.name },
        { QStringLiteral("collectionType"), library.collectionType },
        { QStringLiteral("imageTag"), library.imageTag },
        { QStringLiteral("item"), QVariant::fromValue(library) },
    };
}

void LibraryListModel::setLibraries(const std::vector<LibraryItem>& libraries)
{
    const int oldCount = rowCount();
    beginResetModel();
    m_libraries = libraries;
    endResetModel();
    if (oldCount != rowCount())
        emit countChanged();
}

void LibraryListModel::clear()
{
    const int oldCount = rowCount();
    beginResetModel();
    m_libraries.clear();
    endResetModel();
    if (oldCount != 0)
        emit countChanged();
}

LibraryItem LibraryListModel::libraryAt(int index) const
{
    if (index < 0 || index >= rowCount())
        return {};
    return m_libraries[static_cast<size_t>(index)];
}

const std::vector<LibraryItem>& LibraryListModel::libraries() const
{
    return m_libraries;
}

} // namespace Spool
