#include "LibraryPrefetchController.h"

#include "../common/AsyncTask.h"
#include "ArtworkPrefetcher.h"
#include "LibraryQuery.h"

#include <QDateTime>
#include <QDebug>

#include <algorithm>
#include <utility>

namespace Spool {

namespace {

    constexpr int kLibraryPageSize = 100;
    constexpr int kBackgroundLibraryPrefetchLimit = 3;

} // namespace

LibraryPrefetchController::LibraryPrefetchController(Catalog *catalog, ArtworkPrefetcher *artwork, QObject *parent)
    : QObject(parent)
    , m_api(catalog)
    , m_artwork(artwork)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &LibraryPrefetchController::startNext);
}

void LibraryPrefetchController::stop()
{
    m_generation.invalidate();
    m_timer.stop();
    m_queue.clear();
    m_index = 0;
    m_active = false;
}

void LibraryPrefetchController::schedule(const std::vector<LibraryItem>& libraries, const QStringList& recentLibraryIds)
{
    stop();
    if (!m_api || !m_api->signedIn())
        return;

    std::vector<LibraryItem> selected;
    selected.reserve(kBackgroundLibraryPrefetchLimit);
    QSet<QString> selectedIds;

    auto trySelect = [&selected, &selectedIds](const LibraryItem& library) {
        if (selected.size() >= static_cast<size_t>(kBackgroundLibraryPrefetchLimit))
            return;
        if (!supportsLatestLibraryRow(library) || selectedIds.contains(library.id))
            return;
        selectedIds.insert(library.id);
        selected.push_back(library);
    };

    for (const QString& recentId : recentLibraryIds) {
        const auto it = std::find_if(libraries.begin(), libraries.end(),
            [&recentId](const LibraryItem& library) { return library.id == recentId; });
        if (it != libraries.end())
            trySelect(*it);
    }
    for (const LibraryItem& library : libraries)
        trySelect(library);

    QSet<QString> retainedKeys;
    for (const LibraryItem& library : selected) {
        const QString key = libraryCacheKey(library);
        retainedKeys.insert(key);
        if (!m_cachedKeys.contains(key)) {
            m_queue.push_back(PrefetchRequest {
                BrowseDescriptor::library(library.id, library.collectionType, library.name),
                key,
                library.name,
            });
        }
    }

    for (auto it = m_pages.begin(); it != m_pages.end();) {
        if (retainedKeys.contains(it.key())) {
            ++it;
        } else {
            m_pageStoredAtMs.remove(it.key());
            it = m_pages.erase(it);
        }
    }
    m_cachedKeys.intersect(retainedKeys);

    if (m_queue.empty())
        return;

    qInfo() << "library prefetch: scheduled" << m_queue.size() << "libraries for post-launch idle";
    m_timer.start(10000);
}

std::optional<PagedMovieItems> LibraryPrefetchController::cachedPage(const QString& cacheKey) const
{
    const auto it = m_pages.constFind(cacheKey);
    if (it == m_pages.constEnd())
        return std::nullopt;
    return it.value();
}

void LibraryPrefetchController::storePage(const QString& cacheKey, const PagedMovieItems& page)
{
    if (cacheKey.isEmpty())
        return;
    m_pages.insert(cacheKey, page);
    m_pageStoredAtMs.insert(cacheKey, QDateTime::currentMSecsSinceEpoch());
    m_cachedKeys.insert(cacheKey);
}

qint64 LibraryPrefetchController::pageAgeMs(const QString& cacheKey) const
{
    const auto it = m_pageStoredAtMs.constFind(cacheKey);
    if (it == m_pageStoredAtMs.constEnd())
        return -1;
    return std::max<qint64>(0, QDateTime::currentMSecsSinceEpoch() - it.value());
}

void LibraryPrefetchController::prefetchPosters(
    const std::vector<MovieItem>& items, int firstIndex, int visibleCount, ImageKind imageKind)
{
    if (items.empty() || !m_artwork)
        return;

    const int begin = std::clamp(firstIndex, 0, static_cast<int>(items.size()));
    const int windowSize = std::max(1, visibleCount) + m_imagePrefetchAheadItems;
    const int end = std::min(static_cast<int>(items.size()), begin + windowSize);
    QStringList urls;
    urls.reserve(end - begin);

    for (int index = begin; index < end; ++index) {
        const MovieItem& item = items[static_cast<size_t>(index)];
        const QString url = m_artwork->itemUrl(item, imageKind == ImageKind::Landscape);
        if (!url.isEmpty())
            urls.push_back(url);
    }

    if (!urls.isEmpty())
        m_artwork->prefetch(urls);
}

void LibraryPrefetchController::startNext()
{
    if (m_active || !m_api || !m_api->signedIn())
        return;
    if (m_index < 0 || m_index >= static_cast<int>(m_queue.size()))
        return;

    const RequestGeneration::Token generation = m_generation.next();
    const PrefetchRequest request = m_queue[static_cast<size_t>(m_index++)];
    m_active = true;
    qInfo() << "library prefetch: fetching" << request.title << request.cacheKey;

    Async::runLatest(
        this, m_api->fetchBrowsePage(request.descriptor, 0, kLibraryPageSize), m_generation, generation,
        [this, request](const PagedMovieItems& page) {
            qInfo() << "library prefetch: cached" << request.title << page.items.size();
            storePage(request.cacheKey, page);
            m_active = false;
            m_timer.start(2000);
        },
        [this, request](const std::exception_ptr& error) {
            qWarning() << "library prefetch: failed" << request.title << request.cacheKey << exceptionMessage(error);
            m_active = false;
            m_timer.start(2000);
        });
}

} // namespace Spool
