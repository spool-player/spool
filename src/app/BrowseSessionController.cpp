#include "BrowseSessionController.h"

#include "LibraryPrefetchController.h"
#include "LibraryQuery.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QSettings>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Spool {

namespace {

    // Block fetches in flight at once. Providers allow eight operations per
    // account; leaving room keeps artwork, details and search responsive.
    constexpr int kMaxPendingBlocks = 3;
    // A view that has just jumped waits this long for its next move before
    // loading, so stepping through letters loads only where it stops.
    constexpr int kJumpSettleMs = 150;
    constexpr int kLeadBlockDelayMs = 90;
    // Guesses a type-to-seek makes before settling on the nearest row.
    constexpr int kMaxSeekSteps = 10;

    bool supportsSparse(BrowseKind kind)
    {
        return kind == BrowseKind::Library || kind == BrowseKind::Genre || kind == BrowseKind::Studio;
    }

    QString itemSortName(const MovieItem& item)
    {
        return item.sortName.isEmpty() ? item.title : item.sortName;
    }

    // Where a folded sort key falls in [0, 1) by its first few characters:
    // the ruler a seek uses to guess between two rows it already knows.
    double keyPosition(const QString& key)
    {
        double position = 0;
        double scale = 1;
        for (int i = 0; i < std::min<qsizetype>(4, key.size()); ++i) {
            const char16_t c = key.at(i).unicode();
            const int value = c < u'a' ? 0 : c > u'z' ? 27 : c - u'a' + 1;
            scale /= 28;
            position += value * scale;
        }
        return position;
    }

} // namespace

BrowseSessionController::BrowseSessionController(LibraryPrefetchController *prefetch, QObject *parent)
    : QObject(parent)
    , m_prefetch(prefetch)
{
    // Lead blocks wait for scrolling to pause; a fling across a library must
    // not queue a fetch for every block it passes.
    m_blockTimer.setSingleShot(true);
    connect(&m_blockTimer, &QTimer::timeout, this, &BrowseSessionController::pumpBlocks);
}

bool BrowseSessionController::alphabetical() const
{
    if (!supportsSparse(m_descriptor.kind))
        return false;
    const QString sortBy = m_query.value(QStringLiteral("sortBy")).toString();
    return sortBy.isEmpty() || sortBy == QLatin1String("SortName");
}

void BrowseSessionController::noteAlphabetChanged()
{
    ++m_alphabetRevision;
    emit alphabetChanged();
}

bool BrowseSessionController::sortDescending() const
{
    return m_query.value(QStringLiteral("sortOrder")).toString() == QLatin1String("Descending");
}

QVariantList BrowseSessionController::alphabet() const
{
    QVariantList result;
    if (!alphabetical() || m_items.rowCount() <= 0)
        return result;
    const auto& letters = m_index.letters();
    result.reserve(static_cast<qsizetype>(letters.size()));
    for (size_t i = 0; i < letters.size(); ++i) {
        const auto& letter = letters[i];
        const int end = i + 1 < letters.size() ? letters[i + 1].row : m_items.rowCount();
        // A letter known to be empty has nowhere to jump to.
        if (end <= letter.row && letter.exact)
            continue;
        result.push_back(QVariantMap { { QStringLiteral("label"), letter.label }, { QStringLiteral("row"), letter.row },
            { QStringLiteral("exact"), letter.exact } });
    }
    return result;
}

QString BrowseSessionController::labelAt(int row) const
{
    if (m_items.isLoaded(row))
        return BrowseIndex::labelFor(itemSortName(m_items.movieAt(row)));
    return m_index.labelAt(row);
}

int BrowseSessionController::rowForLabel(const QString& label) const
{
    return m_index.rowForLabel(label);
}

void BrowseSessionController::setLoadedRowBudget(int rows)
{
    m_loadedRowBudget = std::max(400, rows);
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
        setPage(*prefetchedPage, cacheKey, false);
        return m_items.rowCount();
    }
    m_items.clear();
    return 0;
}

void BrowseSessionController::clear()
{
    resetSparseState();
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
    m_nextCursor.reset();
    m_seenCursors.clear();
    resetSparseState();
    emit pagingChanged();
}

void BrowseSessionController::resetSparseState()
{
    m_blockTimer.stop();
    m_loadedBlocks.clear();
    m_pendingBlocks.clear();
    m_abandonedBlocks.clear();
    m_wantedBlocks.clear();
    m_lastScheduledBlock = 0;
    m_viewFirst = 0;
    m_viewLast = 0;
    m_index.reset(0, false);
    cancelSeek();
    noteAlphabetChanged();
}

void BrowseSessionController::observeRows(int first, const std::vector<MovieItem>& items)
{
    if (!alphabetical())
        return;
    for (size_t offset = 0; offset < items.size(); ++offset)
        m_index.observe(first + static_cast<int>(offset), itemSortName(items[offset]));
}

void BrowseSessionController::beginSparseListing(const PagedMovieItems& page)
{
    resetSparseState();
    m_blockSize = std::max(1, page.limit > 0 ? page.limit : static_cast<int>(page.items.size()));
    m_items.beginSparse(page.totalRecordCount);
    m_items.setRows(page.startIndex, page.items);
    m_loadedBlocks[page.startIndex - page.startIndex % m_blockSize] = ++m_useClock;
    m_index.reset(page.totalRecordCount, sortDescending());
    observeRows(page.startIndex, page.items);
    qInfo() << "browse page: sparse listing" << "rows=" << page.totalRecordCount << "block=" << m_blockSize;
    noteAlphabetChanged();
    if (alphabetical())
        emit indexRequested();
}

void BrowseSessionController::setPage(const PagedMovieItems& page, const QString& cacheKey, bool append)
{
    QElapsedTimer applyTimer;
    applyTimer.start();
    if (!page.exhausted
        && (!page.nextCursor || page.nextCursor->isEmpty() || (append && m_seenCursors.contains(*page.nextCursor)))) {
        m_loadingMore = false;
        m_hasMore = false;
        emit pagingChanged();
        throw std::runtime_error("invalid_pagination");
    }
    if (!append)
        m_seenCursors.clear();
    if (!page.exhausted)
        m_seenCursors.insert(*page.nextCursor);
    m_nextCursor = page.exhausted ? std::nullopt : page.nextCursor;

    const bool sparseListing = !append && page.seekable && page.startIndex == 0 && supportsSparse(m_descriptor.kind)
        && page.totalRecordCount > static_cast<int>(page.items.size());
    if (sparseListing) {
        // A refresh of the same listing keeps the rows already loaded.
        if (m_items.isSparse() && m_items.rowCount() == page.totalRecordCount && m_cacheKey == cacheKey) {
            m_items.setRows(0, page.items);
            observeRows(0, page.items);
        } else {
            beginSparseListing(page);
        }
        m_cacheKey = cacheKey;
        m_nextStartIndex = page.totalRecordCount;
        m_totalCount = page.totalRecordCount;
        if (page.limit > 0)
            m_pageSize = page.limit;
        m_hasMore = false;
        m_loadingMore = false;
        if (m_prefetch)
            m_prefetch->prefetchPosters(page.items);
        emit pagingChanged();
        return;
    }

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
    m_hasMore = !page.exhausted;
    m_loadingMore = false;
    // A whole listing in hand is its own exact index.
    if (!append || m_index.count() != m_items.rowCount()) {
        m_index.reset(m_items.rowCount(), sortDescending());
        observeRows(0, m_items.movies());
    } else {
        observeRows(page.startIndex, page.items);
    }
    noteAlphabetChanged();

    if (m_prefetch)
        m_prefetch->prefetchPosters(page.items);
    emit pagingChanged();
}

void BrowseSessionController::applyBlock(int start, const PagedMovieItems& page, const QString& cacheKey)
{
    m_pendingBlocks.erase(std::remove(m_pendingBlocks.begin(), m_pendingBlocks.end(), start), m_pendingBlocks.end());
    m_abandonedBlocks.erase(start);
    if (!m_items.isSparse() || cacheKey != m_cacheKey)
        return;
    if (page.totalRecordCount > 0 && page.totalRecordCount != m_items.rowCount()) {
        // The library changed underneath; start the listing over from here.
        qInfo() << "browse page: sparse listing changed size" << m_items.rowCount() << "->" << page.totalRecordCount;
        PagedMovieItems restart = page;
        restart.startIndex = start;
        beginSparseListing(restart);
        m_totalCount = page.totalRecordCount;
        m_nextStartIndex = page.totalRecordCount;
        emit pagingChanged();
        return;
    }
    m_items.setRows(start, page.items);
    m_loadedBlocks[start] = ++m_useClock;
    if (!m_index.exact()) {
        observeRows(start, page.items);
        noteAlphabetChanged();
    }
    evictBlocks();
    const bool viewportPending = std::any_of(m_pendingBlocks.begin(), m_pendingBlocks.end(),
        [this](int block) { return block + m_blockSize > m_viewFirst && block <= m_viewLast; });
    if (m_loadingMore != viewportPending) {
        m_loadingMore = viewportPending;
        emit pagingChanged();
    }
    if (!m_seekKey.isEmpty() && m_seekWaitingFor == start)
        stepSeek();
    pumpBlocks();
}

void BrowseSessionController::blockFailed(int start)
{
    // The fetch given up on, not one asked for again since.
    if (m_abandonedBlocks.erase(start) > 0)
        return;
    m_pendingBlocks.erase(std::remove(m_pendingBlocks.begin(), m_pendingBlocks.end(), start), m_pendingBlocks.end());
    if (!m_seekKey.isEmpty() && m_seekWaitingFor == start)
        cancelSeek();
    if (m_loadingMore && m_pendingBlocks.empty()) {
        m_loadingMore = false;
        emit pagingChanged();
    }
}

void BrowseSessionController::setExactIndex(const std::vector<BrowseIndexEntry>& entries, const QString& cacheKey)
{
    if (cacheKey != m_cacheKey || !alphabetical())
        return;
    if (!entries.empty()) {
        m_index.setExact(entries);
        noteAlphabetChanged();
        return;
    }
    // No counts from the server: the far end of the listing is the anchor
    // that makes the estimate worth having.
    if (m_items.isSparse()) {
        const int last = (m_items.rowCount() - 1) / m_blockSize * m_blockSize;
        if (!m_loadedBlocks.contains(last)) {
            m_wantedBlocks.push_back(last);
            pumpBlocks();
        }
    }
}

void BrowseSessionController::scheduleBlocks()
{
    const int count = m_items.rowCount();
    if (!m_items.isSparse() || count <= 0)
        return;
    const int firstBlock = std::clamp(m_viewFirst, 0, count - 1) / m_blockSize;
    const int lastBlock = std::clamp(m_viewLast, 0, count - 1) / m_blockSize;
    const int center = (firstBlock + lastBlock) / 2;
    std::deque<int> wanted;
    auto want = [&](int block) {
        const int start = block * m_blockSize;
        if (block < 0 || start >= count || m_loadedBlocks.contains(start))
            return;
        if (std::find(wanted.begin(), wanted.end(), start) == wanted.end())
            wanted.push_back(start);
    };
    // On screen from the middle out, then one block either side.
    for (int distance = 0; center - distance >= firstBlock || center + distance <= lastBlock; ++distance) {
        if (center + distance <= lastBlock)
            want(center + distance);
        if (distance > 0 && center - distance >= firstBlock)
            want(center - distance);
    }
    want(lastBlock + 1);
    want(firstBlock - 1);
    // A pending seek's block and the far-end anchor stay wanted.
    for (const int start : m_wantedBlocks) {
        if (start == m_seekWaitingFor || start == (count - 1) / m_blockSize * m_blockSize)
            want(start / m_blockSize);
    }
    m_wantedBlocks = std::move(wanted);
    for (int block = firstBlock; block <= lastBlock; ++block) {
        if (const auto loaded = m_loadedBlocks.find(block * m_blockSize); loaded != m_loadedBlocks.end())
            loaded->second = ++m_useClock;
    }
    // Fetches the view has left by more than a couple of blocks give up their
    // slots, so a jump is not stuck behind rows nobody is looking at any more.
    const int anchor = (count - 1) / m_blockSize * m_blockSize;
    for (auto it = m_pendingBlocks.begin(); it != m_pendingBlocks.end();) {
        const int block = *it / m_blockSize;
        if (*it > 0 && (block < firstBlock - 2 || block > lastBlock + 2) && *it != m_seekWaitingFor && *it != anchor) {
            const int start = *it;
            it = m_pendingBlocks.erase(it);
            m_abandonedBlocks.insert(start);
            emit blockAbandoned(start);
        } else {
            ++it;
        }
    }
    const bool jumped = std::abs(firstBlock - m_lastScheduledBlock) > lastBlock - firstBlock + 2;
    m_lastScheduledBlock = firstBlock;
    const bool viewportMissing = std::any_of(m_wantedBlocks.begin(), m_wantedBlocks.end(),
        [&](int start) { return start / m_blockSize >= firstBlock && start / m_blockSize <= lastBlock; });
    if (viewportMissing && !jumped)
        pumpBlocks();
    else
        m_blockTimer.start(jumped ? kJumpSettleMs : kLeadBlockDelayMs);
}

void BrowseSessionController::pumpBlocks()
{
    while (static_cast<int>(m_pendingBlocks.size()) < kMaxPendingBlocks && !m_wantedBlocks.empty()) {
        const int start = m_wantedBlocks.front();
        m_wantedBlocks.pop_front();
        if (m_loadedBlocks.contains(start)
            || std::find(m_pendingBlocks.begin(), m_pendingBlocks.end(), start) != m_pendingBlocks.end())
            continue;
        m_pendingBlocks.push_back(start);
        if (start + m_blockSize > m_viewFirst && start <= m_viewLast && !m_loadingMore) {
            m_loadingMore = true;
            emit pagingChanged();
        }
        emit blockRequested(start);
    }
}

void BrowseSessionController::evictBlocks()
{
    const int budget = std::max(4, m_loadedRowBudget / m_blockSize);
    const int keepFirst = m_viewFirst - 2 * m_blockSize;
    const int keepLast = m_viewLast + 2 * m_blockSize;
    const int center = (m_viewFirst + m_viewLast) / 2;
    while (static_cast<int>(m_loadedBlocks.size()) > budget) {
        auto victim = m_loadedBlocks.end();
        int victimDistance = -1;
        for (auto it = m_loadedBlocks.begin(); it != m_loadedBlocks.end(); ++it) {
            const int start = it->first;
            // The opening block is the warm cache's page; keep it.
            if (start == 0 || (start + m_blockSize > keepFirst && start <= keepLast))
                continue;
            const int distance = std::abs(start + m_blockSize / 2 - center);
            if (distance > victimDistance) {
                victim = it;
                victimDistance = distance;
            }
        }
        if (victim == m_loadedBlocks.end())
            return;
        m_items.unloadRows(victim->first, victim->first + m_blockSize - 1);
        m_loadedBlocks.erase(victim);
    }
}

bool BrowseSessionController::seekPrefix(const QString& text)
{
    if (!alphabetical() || (!m_items.isSparse() && m_hasMore))
        return false;
    const QString key = BrowseIndex::sortKey(text);
    if (key.isEmpty() || m_items.rowCount() <= 0)
        return false;
    m_seekKey = key;
    m_seekSteps = 0;
    m_seekWaitingFor = -1;
    emit seekingChanged();
    stepSeek();
    return true;
}

void BrowseSessionController::cancelSeek()
{
    const bool was = !m_seekKey.isEmpty();
    m_seekKey.clear();
    m_seekWaitingFor = -1;
    m_seekSteps = 0;
    if (was)
        emit seekingChanged();
}

void BrowseSessionController::stepSeek()
{
    if (m_seekKey.isEmpty())
        return;
    const QString target = m_seekKey;
    const bool descending = sortDescending();
    const int count = m_items.rowCount();
    // Rows at or after where the target's run begins, in listing order.
    auto atOrAfter
        = [&](const QString& key) { return descending ? (key < target || key.startsWith(target)) : key >= target; };

    // The tightest bracket the loaded rows give.
    int lo = -1;
    int hi = count;
    QString loKey;
    QString hiKey;
    const auto& loaded = m_items.movies();
    for (int slot = 0; slot < static_cast<int>(loaded.size()); ++slot) {
        const int row = m_items.rowOfSlot(slot);
        const QString key = BrowseIndex::sortKey(itemSortName(loaded[static_cast<size_t>(slot)]));
        if (atOrAfter(key)) {
            if (row < hi) {
                hi = row;
                hiKey = key;
            }
        } else if (row > lo) {
            lo = row;
            loKey = key;
        }
    }

    const auto settle = [this, count](int row) {
        const int settled = std::clamp(row, 0, count - 1);
        cancelSeek();
        emit seekMoved(settled, true);
    };
    // Adjacent, or crossed where the server collates differently from us.
    if (hi <= lo + 1) {
        settle(hi);
        return;
    }
    if (++m_seekSteps > kMaxSeekSteps) {
        settle(hi);
        return;
    }

    // Narrow by the letter's span, then interpolate on the key's characters.
    const double targetPosition = keyPosition(target);
    const double ascendingStart = keyPosition(target.left(1));
    const auto [letterStart, letterEnd] = m_index.spanForLabel(BrowseIndex::labelFor(target));
    struct Bound {
        int row;
        double position;
    };
    Bound low { lo + 1, lo >= 0 ? keyPosition(loKey) : (descending ? 1.0 : 0.0) };
    Bound high { hi, hi < count ? keyPosition(hiKey) : (descending ? 0.0 : 1.0) };
    const double letterLowPosition = descending ? ascendingStart + 1.0 / 28 : ascendingStart;
    const double letterHighPosition = descending ? ascendingStart : ascendingStart + 1.0 / 28;
    if (letterStart > low.row && letterStart < high.row)
        low = { letterStart, letterLowPosition };
    if (letterEnd < high.row && letterEnd > low.row)
        high = { letterEnd, letterHighPosition };
    double share = 0.5;
    if (std::abs(high.position - low.position) > 1e-12)
        share = std::clamp((targetPosition - low.position) / (high.position - low.position), 0.0, 1.0);
    if (target.size() == 1 && letterStart > lo && letterStart < hi)
        share = 0;
    const int guess = std::clamp(
        static_cast<int>(std::lround(low.row + share * (high.row - low.row))), lo + 1, std::max(lo + 1, hi - 1));
    const int block = guess / m_blockSize * m_blockSize;
    // The view follows the guess, so that is where rows must stay loaded.
    const int span = std::max(0, m_viewLast - m_viewFirst);
    m_viewFirst = guess;
    m_viewLast = std::min(count - 1, guess + span);
    emit seekMoved(guess, false);
    if (!m_items.isSparse() || m_loadedBlocks.contains(block)) {
        settle(guess);
        return;
    }
    m_seekWaitingFor = block;
    m_wantedBlocks.erase(std::remove(m_wantedBlocks.begin(), m_wantedBlocks.end(), block), m_wantedBlocks.end());
    m_wantedBlocks.push_front(block);
    if (std::find(m_pendingBlocks.begin(), m_pendingBlocks.end(), block) == m_pendingBlocks.end()) {
        // A seek is what the viewer is waiting on; it goes ahead of the queue.
        m_pendingBlocks.push_back(block);
        m_wantedBlocks.pop_front();
        emit blockRequested(block);
    }
}

void BrowseSessionController::setLoadingMore(bool loading)
{
    if (m_loadingMore == loading)
        return;
    m_loadingMore = loading;
    emit pagingChanged();
}

void BrowseSessionController::prefetchVisibleRange(int firstIndex, int lastIndex)
{
    if (firstIndex < 0 || lastIndex < firstIndex || m_items.rowCount() <= 0)
        return;
    if (m_items.isSparse()) {
        m_viewFirst = firstIndex;
        m_viewLast = lastIndex;
        scheduleBlocks();
        if (m_prefetch) {
            const std::vector<MovieItem> visible = m_items.loadedRange(firstIndex, lastIndex + 16);
            m_prefetch->prefetchPosters(
                visible, 0, static_cast<int>(visible.size()), LibraryPrefetchController::ImageKind::Poster, true);
        }
        return;
    }
    if (m_prefetch)
        m_prefetch->prefetchPosters(m_items.movies(), firstIndex, lastIndex - firstIndex + 1,
            LibraryPrefetchController::ImageKind::Poster, true);
    prefetchPageForIndex(lastIndex);
}

void BrowseSessionController::prefetchPageForIndex(int lastIndex)
{
    if (m_pageSize <= 0 || lastIndex < 0 || m_items.isSparse())
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
    resetPaging();
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
    resetPaging();
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
    resetPaging();
    m_libraryId.clear();
    m_libraryCollectionType.clear();
    m_seriesId.clear();
    m_seasonId.clear();
    m_descriptor = {};
}

} // namespace Spool
