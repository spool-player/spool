#pragma once

#include "../media/MediaTypes.h"
#include "../models/MovieGridModel.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <vector>

namespace Spool {

class LibraryPrefetchController;

class BrowseSessionController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(Spool::MovieGridModel *items READ items CONSTANT)
    Q_PROPERTY(bool loadingMore READ loadingMore NOTIFY pagingChanged)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY pagingChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY pagingChanged)
    Q_PROPERTY(QString libraryId READ libraryId NOTIFY changed)
    Q_PROPERTY(QString libraryCollectionType READ libraryCollectionType NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString viewKind READ viewKind NOTIFY changed)
    Q_PROPERTY(QVariantMap query READ query NOTIFY changed)
    Q_PROPERTY(QVariantMap filterOptions READ filterOptions NOTIFY changed)
    Q_PROPERTY(int filterActiveCount READ filterActiveCount NOTIFY changed)

public:
    explicit BrowseSessionController(LibraryPrefetchController *prefetch, QObject *parent = nullptr);

    MovieGridModel *items()
    {
        return &m_items;
    }
    QString cacheKey() const
    {
        return m_cacheKey;
    }
    bool loadingMore() const
    {
        return m_loadingMore;
    }
    bool hasMore() const
    {
        return m_hasMore;
    }
    int totalCount() const
    {
        return m_totalCount;
    }
    int nextStartIndex() const
    {
        return m_nextStartIndex;
    }
    int rowCount() const
    {
        return m_items.rowCount();
    }
    QString libraryId() const
    {
        return m_libraryId;
    }
    QString libraryCollectionType() const
    {
        return m_libraryCollectionType;
    }
    QString title() const
    {
        return m_title;
    }
    QString viewKind() const
    {
        return m_viewKind;
    }
    QString seriesId() const
    {
        return m_seriesId;
    }
    QString seasonId() const
    {
        return m_seasonId;
    }
    QVariantMap query() const
    {
        return m_query;
    }
    QVariantMap filterOptions() const
    {
        return m_filterOptions;
    }
    int filterActiveCount() const;
    BrowseDescriptor descriptor() const
    {
        return m_descriptor;
    }
    MovieItem itemAt(int index) const
    {
        return m_items.movieAt(index);
    }
    int applyCachedPage(const QString& cacheKey);
    void clear();
    void reset();
    void resetPaging(const QString& cacheKey = {});
    void setPage(const PagedMovieItems& page, const QString& cacheKey, bool append);
    void setLoadingMore(bool loading);
    void setWarmCachePaging(int cachedCount, int pageSize);
    Q_INVOKABLE void prefetchVisibleRange(int firstIndex, int lastIndex);
    Q_INVOKABLE void prefetchPageForIndex(int lastIndex);
    Q_INVOKABLE void prefetchNextPage();
    Q_INVOKABLE QVariantMap mediaInfoFor(int index, const QString& preferredAudioLanguage) const;
    void updateResumeTicks(const QString& itemId, qint64 positionTicks);
    void updateFavorite(const QString& itemId, bool favorite);
    void updatePlayed(const QString& itemId, bool played);

    void enterLibrary(const LibraryItem& library, const QVariantMap& defaultQuery);
    bool enterItem(const MovieItem& item);
    void enterNamedCollection(const QString& viewKind, const QString& name, const QString& collectionType = {});
    Q_INVOKABLE void setSort(const QString& sortBy, const QString& sortOrder);
    Q_INVOKABLE void setQueryListValue(const QString& key, const QString& value, bool enabled);
    Q_INVOKABLE void setQueryValue(const QString& key, const QVariant& value);
    Q_INVOKABLE void clearFilters();
    bool setQuery(QVariantMap query);
    void clearFilterOptions();
    void setFilterOptions(const QVariantMap& options);

signals:
    void pagingChanged();
    void changed();
    void reloadRequested();
    void moreItemsRequested();

private:
    void clearBrowseIdentity();
    void requestQuery(QVariantMap query);

    LibraryPrefetchController *m_prefetch = nullptr;
    MovieGridModel m_items;
    QString m_cacheKey;
    bool m_loadingMore = false;
    bool m_hasMore = false;
    int m_totalCount = 0;
    int m_nextStartIndex = 0;
    int m_pageSize = 0;

    QString m_libraryId;
    QString m_libraryCollectionType;
    QString m_title;
    QString m_viewKind;
    QString m_seriesId;
    QString m_seasonId;
    BrowseDescriptor m_descriptor;
    QHash<QString, QVariantMap> m_libraryQueries;
    QVariantMap m_query;
    QVariantMap m_filterOptions;
};

} // namespace Spool
