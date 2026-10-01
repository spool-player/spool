#pragma once

#include "../common/RequestGeneration.h"
#include "../media/MediaTypes.h"
#include "../provider/Catalog.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <optional>
#include <vector>

namespace Spool {

class ArtworkPrefetcher;

class LibraryPrefetchController final : public QObject {
    Q_OBJECT

public:
    enum class ImageKind { Poster, Landscape };

    LibraryPrefetchController(Catalog *catalog, ArtworkPrefetcher *artwork = nullptr, QObject *parent = nullptr);

    void stop();
    void schedule(const std::vector<LibraryItem>& libraries, const QStringList& recentLibraryIds);
    std::optional<PagedMovieItems> cachedPage(const QString& cacheKey) const;
    void storePage(const QString& cacheKey, const PagedMovieItems& page);
    // Milliseconds since the cached page was stored, or -1 when absent.
    qint64 pageAgeMs(const QString& cacheKey) const;
    void prefetchPosters(const std::vector<MovieItem>& items, int firstIndex = 0, int visibleCount = 12,
        ImageKind imageKind = ImageKind::Poster);

private:
    struct PrefetchRequest {
        BrowseDescriptor descriptor;
        QString cacheKey;
        QString title;
    };

    void startNext();

    Catalog *m_api = nullptr;
    ArtworkPrefetcher *m_artwork = nullptr;
    QTimer m_timer;
    RequestGeneration m_generation;
    int m_index = 0;
    bool m_active = false;
    std::vector<PrefetchRequest> m_queue;
    QHash<QString, PagedMovieItems> m_pages;
    QHash<QString, qint64> m_pageStoredAtMs;
    QSet<QString> m_cachedKeys;
    int m_imagePrefetchAheadItems = 16;
};

} // namespace Spool
