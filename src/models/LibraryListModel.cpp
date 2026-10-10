#include "LibraryListModel.h"

#include <QHash>
#include <QSet>
#include <QSettings>

#include <algorithm>

namespace {
constexpr auto libraryOrderKey = "home/libraryOrder";
constexpr auto hiddenLibrariesKey = "home/hiddenLibraries";
}

namespace Spool {

LibraryListModel::LibraryListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_hiddenIds = QSettings().value(QLatin1String(hiddenLibrariesKey)).toStringList();
}

int LibraryListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_visibleIndices.size());
}

int LibraryListModel::count() const
{
    return rowCount();
}

QVariant LibraryListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
        return {};

    const auto& library = m_libraries[m_visibleIndices[static_cast<size_t>(index.row())]];
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

    const auto& library = m_libraries[m_visibleIndices[static_cast<size_t>(index)]];
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
    rebuildVisibleIndices();
    endResetModel();
    if (oldCount != rowCount())
        emit countChanged();
    emit hiddenLibrariesChanged();
}

bool LibraryListModel::moveLibrary(int from, int to)
{
    if (from < 0 || from >= rowCount() || to < 0 || to >= rowCount() || from == to)
        return false;
    beginMoveRows({}, from, from, {}, to > from ? to + 1 : to);
    // Reorder only visible slots; hidden libraries retain their positions.
    if (from < to) {
        for (int row = from; row < to; ++row)
            std::swap(m_libraries[m_visibleIndices[static_cast<size_t>(row)]],
                m_libraries[m_visibleIndices[static_cast<size_t>(row + 1)]]);
    } else {
        for (int row = from; row > to; --row)
            std::swap(m_libraries[m_visibleIndices[static_cast<size_t>(row)]],
                m_libraries[m_visibleIndices[static_cast<size_t>(row - 1)]]);
    }
    endMoveRows();
    saveOrder();
    return true;
}

void LibraryListModel::saveOrder() const
{
    // Retain slots for temporarily disconnected accounts, including hidden
    // libraries, so reconnecting cannot lose their order.
    QSettings settings;
    QStringList order = settings.value(QLatin1String(libraryOrderKey)).toStringList();
    QSet<QString> connected;
    for (const auto& library : m_libraries)
        connected.insert(library.id);
    size_t next = 0;
    for (QString& id : order) {
        if (connected.contains(id))
            id = m_libraries[next++].id;
    }
    while (next < m_libraries.size())
        order.append(m_libraries[next++].id);
    settings.setValue(QLatin1String(libraryOrderKey), order);
}

void LibraryListModel::rebuildVisibleIndices()
{
    m_visibleIndices.clear();
    m_visibleIndices.reserve(m_libraries.size());
    for (size_t index = 0; index < m_libraries.size(); ++index) {
        if (!isHidden(m_libraries[index].id))
            m_visibleIndices.push_back(index);
    }
}

bool LibraryListModel::isHidden(const QString& libraryId) const
{
    return m_hiddenIds.contains(libraryId);
}

QVariantList LibraryListModel::hiddenLibraries() const
{
    QVariantList result;
    for (const auto& library : m_libraries) {
        if (isHidden(library.id))
            result.append(
                QVariantMap { { QStringLiteral("libraryId"), library.id }, { QStringLiteral("name"), library.name } });
    }
    return result;
}

bool LibraryListModel::hideLibrary(const QString& libraryId)
{
    for (int row = 0; row < count(); ++row) {
        if (m_libraries[m_visibleIndices[static_cast<size_t>(row)]].id != libraryId)
            continue;
        // Snapshot the full order before hiding, even if it was never moved.
        saveOrder();
        beginRemoveRows({}, row, row);
        m_hiddenIds.append(libraryId);
        m_visibleIndices.erase(m_visibleIndices.begin() + row);
        endRemoveRows();
        QSettings().setValue(QLatin1String(hiddenLibrariesKey), m_hiddenIds);
        emit countChanged();
        emit hiddenLibrariesChanged();
        return true;
    }
    return false;
}

bool LibraryListModel::showLibrary(const QString& libraryId)
{
    if (!isHidden(libraryId))
        return false;
    const auto found = std::find_if(
        m_libraries.begin(), m_libraries.end(), [&libraryId](const auto& library) { return library.id == libraryId; });
    if (found == m_libraries.end())
        return false;
    const size_t sourceIndex = static_cast<size_t>(found - m_libraries.begin());
    const auto position = std::lower_bound(m_visibleIndices.begin(), m_visibleIndices.end(), sourceIndex);
    const int row = static_cast<int>(position - m_visibleIndices.begin());
    beginInsertRows({}, row, row);
    m_hiddenIds.removeAll(libraryId);
    m_visibleIndices.insert(position, sourceIndex);
    endInsertRows();
    QSettings().setValue(QLatin1String(hiddenLibrariesKey), m_hiddenIds);
    emit countChanged();
    emit hiddenLibrariesChanged();
    return true;
}

void LibraryListModel::clear()
{
    const int oldCount = rowCount();
    beginResetModel();
    m_libraries.clear();
    m_visibleIndices.clear();
    endResetModel();
    if (oldCount != 0)
        emit countChanged();
    emit hiddenLibrariesChanged();
}

LibraryItem LibraryListModel::libraryAt(int index) const
{
    if (index < 0 || index >= rowCount())
        return {};
    return m_libraries[m_visibleIndices[static_cast<size_t>(index)]];
}

LibraryItem LibraryListModel::libraryById(const QString& libraryId) const
{
    const auto found = std::find_if(
        m_libraries.begin(), m_libraries.end(), [&libraryId](const auto& library) { return library.id == libraryId; });
    return found == m_libraries.end() ? LibraryItem {} : *found;
}

const std::vector<LibraryItem>& LibraryListModel::libraries() const
{
    return m_libraries;
}

} // namespace Spool
