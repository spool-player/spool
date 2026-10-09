#pragma once

#include "../../provider/ArtworkSource.h"
#include "../../provider/Catalog.h"
#include "../../provider/PlaybackSource.h"
#include "../../provider/Provider.h"
#include "../../provider/SearchSource.h"
#include "../../provider/UserItemStateSink.h"

#include <QCoroTask>

#include <QDateTime>
#include <QHash>
#include <QString>

#include <vector>

// Native providers live here, one directory each. Every other provider is
// JavaScript and QML loaded at run time.

namespace Spool {

// Explicitly selected folders form one library. Scanning stays off the GUI
// thread; overlapping roots produce one record per canonical file.
class LocalProvider final : public Provider,
                            public Catalog,
                            public SearchSource,
                            public UserItemStateSink,
                            public ArtworkSource {
    Q_OBJECT

public:
    LocalProvider(QString accountId, QStringList libraryRoots, QObject *parent = nullptr,
        QVariantList downloadedFiles = {}, QString stateRoot = {});
    ~LocalProvider() override;

    QString id() const override;
    QString displayName() const override;
    Capabilities capabilities() const override;
    PlaybackSource *playback() override;
    Catalog *catalog() override
    {
        return this;
    }
    ArtworkSource *artwork() override
    {
        return this;
    }
    SearchSource *search() override
    {
        return this;
    }
    UserItemStateSink *itemState() override
    {
        return this;
    }
    bool ready() const override
    {
        return true;
    }

    // Re-reads the library synchronously; initial construction scans in the background.
    void scan();

    // Catalog, SearchSource, UserItemStateSink
    bool signedIn() const override
    {
        return true;
    }
    QString libraryScopeKey() const override;
    QCoro::Task<PagedMovieItems> fetchBrowsePage(BrowseDescriptor descriptor, int startIndex, int limit,
        QVariantMap queryOptions, std::optional<QString> cursor) override;
    QCoro::Task<MovieItem> fetchItemDetails(QString itemId) override;
    QCoro::Task<std::vector<MovieItem>> fetchSeasons(QString seriesId) override;
    QCoro::Task<std::vector<MovieItem>> fetchEpisodes(QString seriesId, QString seasonId = {}) override;
    QCoro::Task<std::vector<MovieItem>> fetchResumeItems(int limit = 24) override;
    QCoro::Task<std::vector<MovieItem>> fetchNextUpEpisodes(int limit = 24) override;
    QCoro::Task<std::vector<MovieItem>> fetchLatestItems(QString parentId = {}, int limit = 24) override;
    QCoro::Task<std::vector<MovieItem>> fetchSimilarItems(QString itemId, int limit = 24) override;
    QCoro::Task<PersonCredits> fetchItemsByPerson(QString personId, int maximumItems = 4000) override;
    QCoro::Task<std::vector<LibraryItem>> fetchLibraries() override;
    QCoro::Task<QVariantMap> fetchLibraryFilterOptions(QString libraryId, QString collectionType = {}) override;
    QCoro::Task<std::vector<MovieItem>> fetchItemsByIds(QStringList itemIds) override;
    QCoro::Task<std::vector<MovieItem>> searchItems(QString searchTerm, int limit = 80) override;
    QCoro::Task<std::vector<MovieItem>> fetchSearchSuggestions(int limit = 20) override;
    QCoro::Task<void> setItemFavorite(QString itemId, bool favorite) override;
    QCoro::Task<void> setItemPlayed(QString itemId, bool played) override;
    QCoro::Task<void> setItemPlaybackPosition(QString itemId, qint64 positionTicks) override;
    QString imageUrl(const ImageRequest& request) const override;

    // What the player asks for. Sessions come from the scan; reports persist
    // position without undoing played state. Watched uses the item-state sink.
    PlaybackSession playbackSession(const QString& itemId) const;

private:
    struct Record {
        MovieItem item;
        QString path;
        QDateTime modified;
    };
    class Playback;

    static std::vector<Record> scanFolders(const QStringList& folders);
    void setRecords(std::vector<Record> records);
    void restoreState(Record& record) const;
    void persistState(const Record& record) const;
    const Record *record(const QString& itemId) const;
    Record *record(const QString& itemId);
    std::vector<MovieItem> items(int startIndex, int limit) const;

    QString m_accountId;
    QStringList m_roots;
    QVariantList m_downloadedFiles;
    QString m_stateRoot;
    QString m_libraryName;
    std::vector<Record> m_records;
    QHash<QString, size_t> m_index;
    Playback *m_playback = nullptr;
};

} // namespace Spool
