#pragma once

#include "ArtworkSource.h"
#include "Catalog.h"
#include "PlaybackSource.h"
#include "Provider.h"
#include "SearchSource.h"
#include "StreamQualityControl.h"
#include "UserItemStateSink.h"

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace Spool {

class ProviderRegistry;

// The one Provider the app sees: every enabled account behind it at once.
//
// IDs are scoped here and nowhere else. Every ID a source hands out leaves
// as "<8 hex of its account>:<the source's own id>", so routes, caches, the
// queue and QML keep treating IDs as opaque strings and a call carrying one
// finds its way back to the account it came from. Lists that have no single
// owner (libraries, home rows, search) are asked of every account in
// parallel and merged; one slow or failing account never empties the rest.
//
// Accounts set aside for another user of the same server still run for
// search (see searchPlan), but never show up in browsing.
class SourceHub final : public Provider,
                        public Catalog,
                        public SearchSource,
                        public UserItemStateSink,
                        public ArtworkSource,
                        public StreamQualityControl {
    Q_OBJECT
    Q_PROPERTY(bool multipleSources READ multipleSources NOTIFY browseSourcesChanged)

public:
    explicit SourceHub(ProviderRegistry *registry, QObject *parent = nullptr);
    ~SourceHub() override;

    QString id() const override
    {
        return QStringLiteral("hub");
    }
    QString displayName() const override;
    Capabilities capabilities() const override
    {
        return m_capabilities;
    }
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
    StreamQualityControl *streamQuality() override
    {
        return this;
    }
    bool ready() const override;

    // Where a scoped item comes from, for telling libraries on different
    // servers apart: provider name and icon, server name and address.
    Q_INVOKABLE QVariantMap originOf(const QString& scopedId) const;
    bool multipleSources() const;

    // Scoping. `accountOf` answers for any scoped ID, empty when unscoped.
    QString scoped(const QString& accountId, const QString& rawId) const;
    QString accountOf(const QString& scopedId) const;
    static QString rawId(const QString& scopedId);
    Provider *source(const QString& accountId) const;
    // The accounts being browsed; set-aside accounts kept for search are not.
    std::vector<Provider *> sources() const;
    // Calls an operation on the account behind a scoped or account ID.
    QCoro::Task<QVariantMap> call(QString accountId, QString operation, QVariantMap arguments = {});
    void setVideoCodecs(QStringList codecs, bool restrict);

    // Menu policy is fetched only on opening; results are tied to this request.
    Q_INVOKABLE int requestItemActions(
        const QString& itemId, const QString& itemType, const QString& containerId = {}, const QString& entryId = {});
    Q_INVOKABLE void cancelItemActions();
    Q_INVOKABLE void runItemAction(const QString& actionId, const QString& itemId, const QString& itemType,
        const QString& containerId = {}, const QString& entryId = {});
    Q_INVOKABLE bool collectionEditingAvailable(const QString& containerId) const;
    QCoro::Task<QVariantMap> collectionCall(QString containerId, QString operation, QVariantMap arguments = {});
    QCoro::Task<PagedMovieItems> collectionEntries(QString containerId, std::optional<QString> cursor);
    void cancelCollection(const QString& containerId);
    void collectionChanged(const QString& containerId);
    // Outbound remote IDs cross the provider boundary only through this facade.
    bool remoteAvailable(const QString& accountId) const;
    QCoro::Task<QVariantList> remoteTargets(QString accountId, QString scope);
    QCoro::Task<QVariantMap> remoteState(QString targetId, bool connect, QString scope);
    QCoro::Task<QVariantMap> remoteCommand(QString targetId, QVariantMap command, QVariantMap state, QString scope);
    QCoro::Task<PagedMovieItems> remoteQueue(QString targetId, std::optional<QString> cursor, QString scope);
    QVariantMap remoteQueueRow(const MovieItem& item) const;
    // The viewer's standing streaming limits from settings.
    void setPlaybackPreferences(qint64 manualMaxBitrate, bool unlimitedLocalNetwork, bool preferRemux, int maxHeight);
    struct ReportingQueueEntry {
        QString itemId;
        QString entryId;
        bool audio = false;
        friend bool operator==(const ReportingQueueEntry&, const ReportingQueueEntry&) = default;
    };
    void setPlaybackQueue(std::vector<ReportingQueueEntry> items, int index);
    // Idle-only probes are serialized so accounts do not benchmark each other.
    void setPlaybackActive(bool active);
    void refreshSpeedTests();
    QString speedTestDescription() const;

    // Catalog
    bool signedIn() const override;
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

    // SearchSource: every server at once, through as few accounts as reach
    // all of its libraries, ranked together as the answers arrive.
    QCoro::Task<std::vector<MovieItem>> searchItems(QString searchTerm, int limit = 80) override;
    QCoro::Task<void> searchProgressively(QString searchTerm, int limit, SearchUpdate update) override;
    QCoro::Task<std::vector<MovieItem>> fetchSearchSuggestions(int limit = 20) override;
    void prepareSearch() override;

    struct SearchTarget {
        QString accountId;
        // Accounts on one server share item IDs; results dedupe within it.
        QString server;
    };
    // Per server, the fewest accounts whose libraries cover everything any
    // of its signed-in users can see: a user who sees more stands in for one
    // who sees less, and of two who see the same the one in use searches.
    QCoro::Task<std::vector<SearchTarget>> searchPlan();

    // UserItemStateSink
    QCoro::Task<void> setItemFavorite(QString itemId, bool favorite) override;
    QCoro::Task<void> setItemPlayed(QString itemId, bool played) override;
    QCoro::Task<void> setItemPlaybackPosition(QString itemId, qint64 positionTicks) override;

    QString imageUrl(const ImageRequest& request) const override;
    ImageResource resolveImage(const QUrl& url) const override;

    // StreamQualityControl
    qint64 bitrateOverride() const override
    {
        return m_bitrate;
    }
    int heightOverride() const override
    {
        return m_height;
    }
    void setOverride(qint64 bitrate, int height) override;
    QString autoDescription() const override;
    std::vector<Rung> ladder(qint64 sourceBitrate, int sourceHeight = 0) const override
    {
        return defaultLadder(sourceBitrate, sourceHeight);
    }

signals:
    void browseSourcesChanged();
    void accountEvent(const QString& accountId, const QString& type, const QVariantMap& payload);
    void streamingQualityChanged();
    void itemActionsReady(int requestId, const QVariantList& actions, const QString& problem);
    void extensionSupportChanged(const QString& accountId);

private:
    class Playback;
    struct Entry {
        QString accountId;
        QPointer<Provider> provider;
        bool browse = true;
        enum class SpeedState { Pending, Running, Complete, Failed };
        SpeedState speedState = SpeedState::Pending;
        qint64 measuredBitrate = 0;
        int parallelRequests = 2;
        struct RemotePreview {
            QString urlTemplate;
            QByteArray headers;
        };
        QHash<QString, RemotePreview> remotePreviews; // by raw target ID
    };
    struct SearchRun;

    void addSource(Provider *provider);
    void removeSource(const QString& accountId);
    void refresh();
    void pushPlaybackContext();
    void startNextSpeedTest();
    void cancelSpeedTest();
    QString speedDescription(const Entry& entry) const;
    void syncBrowse();
    bool accountEnabled(const QString& accountId) const;
    // The raw IDs of the libraries an account sees; empty when unknown.
    QCoro::Task<std::optional<QSet<QString>>> accessOf(QString accountId);
    QCoro::Task<void> searchOne(std::shared_ptr<SearchRun> run, size_t index, QString searchTerm);
    Provider *owner(const QString& scopedId) const;
    QVariantList baselineItemActions(const QString& itemId, const QString& itemType) const;
    QCoro::Task<QVariantList> fetchItemActions(
        QString itemId, QString itemType, QString containerId, QString entryId, QString scope);
    MovieItem scopedItem(MovieItem item, const QString& accountId) const;
    std::vector<MovieItem> scopedItems(std::vector<MovieItem> items, const QString& accountId) const;
    template <typename Fetch>
    QCoro::Task<std::vector<MovieItem>> gather(Fetch fetch, int limit, bool searchOnly = false);

    ProviderRegistry *m_registry;
    QHash<QString, Entry> m_entries; // by prefix
    Capabilities m_capabilities;
    Playback *m_playback = nullptr;
    QTimer m_settled;
    bool m_announced = false;
    qint64 m_bitrate = 0;
    int m_height = 0;
    QStringList m_videoCodecs;
    bool m_restrictVideoCodecs = false;
    QVariantMap m_preferences;
    std::vector<ReportingQueueEntry> m_reportingQueue;
    QHash<QString, QVariantMap> m_queueSnapshots;
    std::vector<std::pair<QString, int>> m_queueLocations;
    QHash<QString, int> m_queueIndexes;
    quint64 m_queueRevision = 0;
    QTimer m_speedTestTimer;
    QString m_speedTestAccount;
    QString m_playbackAccount;
    quint64 m_speedTestGeneration = 0;
    bool m_playbackActive = false;
    QString m_lastDetailsAccount;
    QHash<QString, QSet<QString>> m_access;
    quint64 m_searchSerial = 0;
    int m_itemActionsRequest = 0;
    QString m_itemActionsAccount;
};

} // namespace Spool
