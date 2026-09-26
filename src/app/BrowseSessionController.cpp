#include "BrowseSessionController.h"

#include "LibraryPrefetchController.h"
#include "LibraryQuery.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QSettings>
#include <algorithm>

namespace Spool {

BrowseSessionController::BrowseSessionController(LibraryPrefetchController *prefetch, QObject *parent)
    : QObject(parent)
    , m_prefetch(prefetch)
{
}

int BrowseSessionController::filterActiveCount() const
{
    return activeLibraryFilterCount(m_query);
}

QVariantMap BrowseSessionController::mediaInfoFor(int index, const QString& preferredAudioLanguage) const
{
    return formatMediaInfo(m_items.movieAt(index), preferredAudioLanguage);
}

int BrowseSessionController::applyCachedPage(const QString& cacheKey)
{
    if (!m_prefetch) {
        m_items.clear();
        return 0;
    }

    const auto prefetchedPage = m_prefetch->cachedPage(cacheKey);
    if (prefetchedPage) {
        m_items.setMovies(prefetchedPage->items);
        m_prefetch->prefetchPosters(prefetchedPage->items);
        return m_items.rowCount();
    }
    m_items.clear();
    return 0;
}

void BrowseSessionController::clear()
{
    m_items.clear();
}

void BrowseSessionController::reset()
{
    clearBrowseIdentity();
    m_libraryQueries.clear();
    m_query.clear();
    m_filterOptions.clear();
    m_items.clear();
    resetPaging();
    emit changed();
}

void BrowseSessionController::resetPaging(const QString& cacheKey)
{
    m_cacheKey = cacheKey;
    m_loadingMore = false;
    m_hasMore = false;
    m_totalCount = 0;
    m_nextStartIndex = 0;
    m_pageSize = 0;
    emit pagingChanged();
}

void BrowseSessionController::setPage(const PagedMovieItems& page, const QString& cacheKey, bool append)
{
    QElapsedTimer applyTimer;
    applyTimer.start();
    if (append)
        m_items.appendMovies(page.items);
    else
        m_items.setMovies(page.items);

    const int loadedCount = m_items.rowCount();
    qInfo() << "browse page: model applied"
            << "start=" << page.startIndex << "append=" << append << "items=" << page.items.size()
            << "loaded=" << loadedCount << "ms=" << applyTimer.elapsed();
    const int pageEnd = page.startIndex + static_cast<int>(page.items.size());
    const bool hasServerTotal = page.totalRecordCount > 0;
    m_cacheKey = cacheKey;
    m_nextStartIndex = std::max(loadedCount, pageEnd);
    m_totalCount = hasServerTotal ? std::max(page.totalRecordCount, m_nextStartIndex) : m_nextStartIndex;
    if (page.limit > 0)
        m_pageSize = page.limit;
    m_hasMore = hasServerTotal ? m_nextStartIndex < m_totalCount
                               : page.items.size() >= static_cast<size_t>(std::max(1, page.limit));
    if (page.items.empty())
        m_hasMore = false;
    m_loadingMore = false;

    if (m_prefetch)
        m_prefetch->prefetchPosters(page.items);
    emit pagingChanged();
}

void BrowseSessionController::setLoadingMore(bool loading)
{
    if (m_loadingMore == loading)
        return;
    m_loadingMore = loading;
    emit pagingChanged();
}

void BrowseSessionController::setWarmCachePaging(int cachedCount, int pageSize)
{
    m_nextStartIndex = cachedCount;
    m_pageSize = std::max(1, pageSize);
    m_totalCount = cachedCount;
    m_hasMore = cachedCount >= pageSize;
    m_loadingMore = true;
    emit pagingChanged();
}

void BrowseSessionController::prefetchVisibleRange(int firstIndex, int lastIndex)
{
    if (firstIndex < 0 || lastIndex < firstIndex || m_items.rowCount() <= 0)
        return;
    if (m_prefetch)
        m_prefetch->prefetchPosters(m_items.movies(), firstIndex, lastIndex - firstIndex + 1);
    prefetchPageForIndex(lastIndex);
}

void BrowseSessionController::prefetchPageForIndex(int lastIndex)
{
    if (m_pageSize <= 0 || lastIndex < 0)
        return;
    // Maintain one and a half pages ahead of the visible tail. Waiting until
    // the highlight enters the newest page leaves too little network headroom
    // for accelerated navigation and visibly stalls at the loaded boundary.
    const int prefetchLead = m_pageSize + std::max(1, m_pageSize / 2);
    if (lastIndex >= std::max(0, m_nextStartIndex - prefetchLead))
        prefetchNextPage();
}

void BrowseSessionController::prefetchNextPage()
{
    if (!m_loadingMore && m_hasMore)
        emit moreItemsRequested();
}

void BrowseSessionController::updateResumeTicks(const QString& itemId, qint64 positionTicks)
{
    m_items.updateResumeTicks(itemId, positionTicks);
}

void BrowseSessionController::updateFavorite(const QString& itemId, bool favorite)
{
    m_items.updateFavorite(itemId, favorite);
}

void BrowseSessionController::updatePlayed(const QString& itemId, bool played)
{
    m_items.updatePlayed(itemId, played);
}

void BrowseSessionController::enterLibrary(const LibraryItem& library, const QVariantMap& defaultQuery)
{
    m_libraryId = library.id;
    m_libraryCollectionType = library.collectionType;
    m_title = library.name;
    m_viewKind = QStringLiteral("library");
    m_seriesId.clear();
    m_seasonId.clear();
    m_descriptor = BrowseDescriptor::library(library.id, library.collectionType, library.name);
    m_query = m_libraryQueries.value(
        library.id, QSettings().value(QStringLiteral("browse/libraryQueries/") + library.id, defaultQuery).toMap());
    m_filterOptions.clear();
    emit changed();
}

bool BrowseSessionController::enterItem(const MovieItem& item)
{
    BrowseDescriptor descriptor;
    QString viewKind;
    if (item.itemType == QStringLiteral("Playlist")) {
        descriptor = BrowseDescriptor::playlist(item.id, item.title);
        viewKind = QStringLiteral("playlist");
    } else if (item.itemType == QStringLiteral("BoxSet")) {
        descriptor = BrowseDescriptor::boxSet(item.id, item.title);
        viewKind = QStringLiteral("boxset");
    } else if (item.itemType == QStringLiteral("Folder") || item.itemType == QStringLiteral("PhotoAlbum")
        || item.itemType == QStringLiteral("MusicAlbum")) {
        descriptor = BrowseDescriptor::folderChildren(item.id, item.title);
        viewKind = QStringLiteral("folder");
    } else if (item.itemType == QStringLiteral("MusicArtist")) {
        descriptor = BrowseDescriptor::artistAlbums(item.id, item.title);
        viewKind = QStringLiteral("artist");
    } else {
        return false;
    }
    clearBrowseIdentity();
    m_descriptor = std::move(descriptor);
    m_viewKind = std::move(viewKind);
    m_title = item.title;
    m_query.clear();
    m_filterOptions.clear();
    emit changed();
    return true;
}

void BrowseSessionController::enterNamedCollection(
    const QString& viewKind, const QString& name, const QString& collectionType)
{
    clearBrowseIdentity();
    if (viewKind == QStringLiteral("genre"))
        m_descriptor = BrowseDescriptor::genre(name, collectionType);
    else if (viewKind == QStringLiteral("studio"))
        m_descriptor = BrowseDescriptor::studio(name);
    else
        m_descriptor = {};
    m_libraryCollectionType = collectionType;
    m_viewKind = viewKind;
    m_title = name;
    m_query.clear();
    m_filterOptions.clear();
    emit changed();
}

void BrowseSessionController::setSort(const QString& sortBy, const QString& sortOrder)
{
    QVariantMap next = m_query;
    next.insert(QStringLiteral("sortBy"), sortBy.isEmpty() ? QStringLiteral("SortName") : sortBy);
    next.insert(QStringLiteral("sortOrder"),
        sortOrder == QStringLiteral("Descending") ? QStringLiteral("Descending") : QStringLiteral("Ascending"));
    requestQuery(std::move(next));
}

void BrowseSessionController::setQueryListValue(const QString& key, const QString& value, bool enabled)
{
    if (key.isEmpty() || value.isEmpty())
        return;
    QVariantMap next = m_query;
    QStringList values = libraryQueryStringList(next, key);
    values.removeAll(value);
    if (enabled)
        values.push_back(value);
    values.removeDuplicates();
    if (values.isEmpty())
        next.remove(key);
    else
        next.insert(key, values);
    requestQuery(std::move(next));
}

void BrowseSessionController::setQueryValue(const QString& key, const QVariant& value)
{
    if (key.isEmpty())
        return;
    QVariantMap next = m_query;
    if (!value.isValid() || value.isNull())
        next.remove(key);
    else
        next.insert(key, value);
    requestQuery(std::move(next));
}

void BrowseSessionController::clearFilters()
{
    if (m_libraryId.isEmpty())
        return;
    QVariantMap next;
    next.insert(
        QStringLiteral("sortBy"), m_query.value(QStringLiteral("sortBy"), QStringLiteral("SortName")).toString());
    next.insert(QStringLiteral("sortOrder"),
        m_query.value(QStringLiteral("sortOrder"), QStringLiteral("Ascending")).toString());
    requestQuery(std::move(next));
}

bool BrowseSessionController::setQuery(QVariantMap query)
{
    if (m_query == query)
        return false;
    m_query = std::move(query);
    if (!m_libraryId.isEmpty()) {
        m_libraryQueries.insert(m_libraryId, m_query);
        QSettings().setValue(QStringLiteral("browse/libraryQueries/") + m_libraryId, m_query);
    }
    emit changed();
    return true;
}

void BrowseSessionController::requestQuery(QVariantMap query)
{
    if (setQuery(std::move(query)))
        emit reloadRequested();
}

void BrowseSessionController::clearFilterOptions()
{
    m_filterOptions.clear();
    emit changed();
}

void BrowseSessionController::setFilterOptions(const QVariantMap& options)
{
    m_filterOptions = options;
    emit changed();
}

void BrowseSessionController::clearBrowseIdentity()
{
    m_libraryId.clear();
    m_libraryCollectionType.clear();
    m_seriesId.clear();
    m_seasonId.clear();
    m_descriptor = {};
}

} // namespace Spool
