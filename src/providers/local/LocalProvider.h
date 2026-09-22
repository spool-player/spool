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

namespace JellyfinNative {

// A deliberately simple media source: every media file under one folder,
// presented as a single library with no server behind it. It exists to
// prove that the shell and the pages need nothing a folder cannot give
// them, and it stays as the contract's permanent test fixture. Favourite,
// played and resume position are kept in memory for the process; nothing is
// reported anywhere.
class LocalProvider final : public Provider,
                            public Catalog,
                            public SearchSource,
                            public UserItemStateSink,
                            public ArtworkSource {
    Q_OBJECT

public:
    LocalProvider(QString accountId, QString libraryRoot, QObject *parent = nullptr);
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

    QString libraryRoot() const
    {
        return m_root;
    }
    // Reads the folder synchronously; construction already does so off the
    // GUI thread, so only tests call this.
    void scan();

    // Catalog, SearchSource, UserItemStateSink
    bool signedIn() const override
    {
        return true;
    }
    QString libraryScopeKey() const override;
    QCoro::Task<PagedMovieItems> fetchBrowsePage(
        BrowseDescriptor descriptor, int startIndex = 0, int limit = 72, QVariantMap queryOptions = {}) override;
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
    // ArtworkSource: a folder has no artwork.
    QString imageUrl(const ImageRequest&) const override
    {
        return {};
    }

    // What the player asks for. Everything is answered from the scan; the
    // report calls complete without doing anything.
    PlaybackSession playbackSession(const QString& itemId) const;

private:
    struct Record {
        MovieItem item;
        QString path;
        QDateTime modified;
    };
    class Playback;

    static std::vector<Record> scanFolder(const QString& folder);
    void setRecords(std::vector<Record> records);
    const Record *record(const QString& itemId) const;
    Record *record(const QString& itemId);
    std::vector<MovieItem> items(int startIndex, int limit) const;

    QString m_accountId;
    QString m_root;
    QString m_libraryName;
    std::vector<Record> m_records;
    QHash<QString, size_t> m_index;
    Playback *m_playback = nullptr;
};

} // namespace JellyfinNative
