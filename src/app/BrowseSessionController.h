#pragma once

#include "../media/MediaTypes.h"
#include "../models/MovieGridModel.h"
#include "BrowseIndex.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <deque>
#include <map>
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
    Q_PROPERTY(QString containerId READ containerId NOTIFY changed)
    Q_PROPERTY(QString libraryCollectionType READ libraryCollectionType NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString viewKind READ viewKind NOTIFY changed)
    Q_PROPERTY(QVariantMap query READ query NOTIFY changed)
    Q_PROPERTY(QVariantMap filterOptions READ filterOptions NOTIFY changed)
    Q_PROPERTY(int filterActiveCount READ filterActiveCount NOTIFY changed)
    // Only the rows near the viewport are held; the rest are placeholders.
    Q_PROPERTY(bool sparse READ sparse NOTIFY pagingChanged)
    // Sorted by name, so letters mean something and typing can seek.
    Q_PROPERTY(bool alphabetical READ alphabetical NOTIFY changed)
    // [{label, row, exact}] in listing order, for the letter rail.
    Q_PROPERTY(QVariantList alphabet READ alphabet NOTIFY alphabetChanged)
    Q_PROPERTY(bool alphabetExact READ alphabetExact NOTIFY alphabetChanged)
    // Bumps whenever letter positions may have moved; cheap to bind to.
    Q_PROPERTY(int alphabetRevision READ alphabetRevision NOTIFY alphabetChanged)
    Q_PROPERTY(bool seeking READ seeking NOTIFY seekingChanged)

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
    std::optional<QString> nextCursor() const
    {
        return m_nextCursor;
    }
    int rowCount() const
    {
        return m_items.rowCount();
    }
    QString libraryId() const
    {
        return m_libraryId;
    }
    QString containerId() const
    {
        return m_descriptor.kind == BrowseKind::Playlist || m_descriptor.kind == BrowseKind::BoxSet ? m_descriptor.id
                                                                                                    : QString();
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
    bool sparse() const
    {
        return m_items.isSparse();
    }
    bool alphabetical() const;
    QVariantList alphabet() const;
    bool alphabetExact() const
    {
        return m_index.exact();
    }
    int alphabetRevision() const
    {
        return m_alphabetRevision;
    }
    bool seeking() const
    {
        return !m_seekKey.isEmpty();
    }
    int blockSize() const
    {
        return m_blockSize;
    }
    // The letter a row is under, known or estimated: what a placeholder and
    // the scrub callout show before the row has loaded.
    Q_INVOKABLE QString labelAt(int row) const;
    Q_INVOKABLE int rowForLabel(const QString& label) const;
    // Moves to the first title starting with `text`, as a file browser does,
    // loading only the rows it needs to find it. False when this listing
    // cannot seek by name; the page then searches what is loaded.
    Q_INVOKABLE bool seekPrefix(const QString& text);
    Q_INVOKABLE void cancelSeek();
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
    // A requested block of a sparse listing arrived, or could not be had.
    void applyBlock(int start, const PagedMovieItems& page, const QString& cacheKey);
    void blockFailed(int start);
    void setExactIndex(const std::vector<BrowseIndexEntry>& entries, const QString& cacheKey);
    // Most rows a sparse listing keeps loaded at once.
    void setLoadedRowBudget(int rows);
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
    // Fetch rows [start, start + blockSize) of the current listing.
    void blockRequested(int start);
    void indexRequested();
    void alphabetChanged();
    void seekingChanged();
    // A seek moved to `row`; `settled` once it is the answer, not a guess.
    void seekMoved(int row, bool settled);

private:
    void clearBrowseIdentity();
    void requestQuery(QVariantMap query);
    void beginSparseListing(const PagedMovieItems& page);
    void resetSparseState();
    void observeRows(int first, const std::vector<MovieItem>& items);
    void scheduleBlocks();
    void pumpBlocks();
    void evictBlocks();
    void stepSeek();
    bool sortDescending() const;
    void noteAlphabetChanged();

    LibraryPrefetchController *m_prefetch = nullptr;
    MovieGridModel m_items;
    QString m_cacheKey;
    bool m_loadingMore = false;
    bool m_hasMore = false;
    int m_totalCount = 0;
    int m_nextStartIndex = 0;
    int m_pageSize = 0;
    std::optional<QString> m_nextCursor;
    QSet<QString> m_seenCursors;

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

    // Sparse listings: blocks are aligned runs of m_blockSize rows.
    int m_blockSize = 100;
    int m_loadedRowBudget = 3000;
    int m_viewFirst = 0;
    int m_viewLast = 0;
    std::map<int, quint64> m_loadedBlocks; // start row -> last use
    std::vector<int> m_pendingBlocks;
    std::deque<int> m_wantedBlocks;
    quint64 m_useClock = 0;
    QTimer m_blockTimer;
    BrowseIndex m_index;
    // Type-to-seek in progress: the folded prefix and how many guesses so far.
    QString m_seekKey;
    int m_seekSteps = 0;
    int m_seekWaitingFor = -1;
    int m_alphabetRevision = 0;
};

} // namespace Spool
