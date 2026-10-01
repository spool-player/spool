#include "LibraryListModel.h"

#include <QHash>
#include <QSet>
#include <QSettings>

#include <algorithm>

namespace {
constexpr auto libraryOrderKey = "home/libraryOrder";
}

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
    const QStringList order = QSettings().value(QLatin1String(libraryOrderKey)).toStringList();
    QHash<QString, qsizetype> positions;
    positions.reserve(order.size());
    for (qsizetype i = 0; i < order.size(); ++i)
        positions.insert(order[i], i);
    std::stable_sort(m_libraries.begin(), m_libraries.end(), [&positions, &order](const auto& a, const auto& b) {
        return positions.value(a.id, order.size()) < positions.value(b.id, order.size());
    });
    endResetModel();
    if (oldCount != rowCount())
        emit countChanged();
}

bool LibraryListModel::moveLibrary(int from, int to)
{
    if (from < 0 || from >= rowCount() || to < 0 || to >= rowCount() || from == to)
        return false;
    beginMoveRows({}, from, from, {}, to > from ? to + 1 : to);
    if (from < to)
        std::rotate(m_libraries.begin() + from, m_libraries.begin() + from + 1, m_libraries.begin() + to + 1);
    else
        std::rotate(m_libraries.begin() + to, m_libraries.begin() + from, m_libraries.begin() + from + 1);
    endMoveRows();

    // Keep slots for temporarily disconnected accounts, replacing only the
    // visible libraries' slots so a refresh or reconnect cannot lose ordering.
    QSettings settings;
    QStringList order = settings.value(QLatin1String(libraryOrderKey)).toStringList();
    QSet<QString> visible;
    for (const auto& library : m_libraries)
        visible.insert(library.id);
    size_t next = 0;
    for (QString& id : order) {
        if (visible.contains(id))
            id = m_libraries[next++].id;
    }
    while (next < m_libraries.size())
        order.append(m_libraries[next++].id);
    settings.setValue(QLatin1String(libraryOrderKey), order);
    return true;
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
