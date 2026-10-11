#pragma once

#include "../common/RequestGeneration.h"
#include "../media/MediaTypes.h"
#include "../models/MovieGridModel.h"
#include "../provider/Catalog.h"
#include "../provider/SourceHub.h"
#include <QCoroTask>
#include <QHash>
#include <QJsonObject>
#include <QSet>

#include <QObject>
#include <QStringList>
#include <QVariantList>

#include <functional>
#include <memory>
#include <vector>

namespace Spool {

class DatabaseManager;
class LibraryPrefetchController;

class SettingsController;
class HomeModelController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(Spool::MovieGridModel *resumeItems READ resumeItems CONSTANT)
    Q_PROPERTY(Spool::MovieGridModel *nextUpItems READ nextUpItems CONSTANT)
    Q_PROPERTY(QVariantList latestLibraryRows READ latestLibraryRows NOTIFY latestLibraryRowsChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    // The providers someone turned off for Home, and whether Home is honouring
    // that right now: it shows everything when nothing that is left on can.
    Q_PROPERTY(QStringList hiddenProviderIds READ hiddenProviderIds NOTIFY providerScopeChanged)
    Q_PROPERTY(bool providerFilterActive READ providerFilterActive NOTIFY providerScopeChanged)
    Q_PROPERTY(QString providerScopeMessage READ providerScopeMessage NOTIFY providerScopeChanged)
    Q_PROPERTY(QVariantList providerChoices READ providerChoices NOTIFY providerScopeChanged)

public:
    HomeModelController(
        DatabaseManager *database, Catalog *catalog, LibraryPrefetchController *prefetch, QObject *parent = nullptr);

    MovieGridModel *resumeItems()
    {
        return &m_resumeItems;
    }
    MovieGridModel *nextUpItems()
    {
        return &m_nextUpItems;
    }
    QVariantList latestLibraryRows() const;
    bool loading() const
    {
        return m_refreshInFlight;
    }

    QStringList hiddenProviderIds() const
    {
        return m_hiddenProviderIds;
    }
    bool providerFilterActive() const
    {
        return !m_homeQuery.hiddenModuleIds.isEmpty();
    }
    QString providerScopeMessage() const
    {
        return m_providerScopeMessage;
    }
    QVariantList providerChoices() const;
    void attachSettings(SettingsController *settings);
    Q_INVOKABLE bool includesItem(const QString& scopedId) const;
    // Refuses to turn off the last provider that can fill Home.
    Q_INVOKABLE bool setProviderShown(const QString& moduleId, bool shown);
    Q_INVOKABLE void showAllProviders();
    bool applyCachedPayload(const QJsonObject& payload);
    void loadCachedPayload();
    void refresh(const std::vector<LibraryItem>& libraries);
    void recordLibraryUse(const LibraryItem& library);
    void upsertResumeItem(MovieItem item, qint64 positionTicks);
    void updateResumeTicks(const QString& itemId, qint64 positionTicks);
    void updateFavorite(const QString& itemId, bool favorite);
    void updatePlayed(const QString& itemId, bool played);
    void advanceNextUp(const MovieItem& completed, const MovieItem& successor);
    void refreshPlaybackRows();
    void invalidate(const std::function<bool(const QString&)>& isAvailable = {});
    void reset();

signals:
    void latestLibraryRowsChanged();
    void loadingChanged();
    void providerScopeChanged();

private:
    struct LatestLibrarySection {
        int order = 0;
        LibraryItem library;
        std::unique_ptr<MovieGridModel> model;
    };
    struct PendingLatestLibrarySection {
        int order = 0;
        LibraryItem library;
        std::vector<MovieItem> items;
    };
    QCoro::Task<void> refreshAsync(std::vector<LibraryItem> libraries, RequestGeneration::Token generation);
    QCoro::Task<std::vector<MovieItem>> fetchLatestLibraryItems(LibraryItem library);
    bool updateLatestLibraryRows(std::vector<PendingLatestLibrarySection> sections);
    QJsonObject payloadFromSections(const std::vector<MovieItem>& resumeItems,
        const std::vector<MovieItem>& nextUpItems, const std::vector<PendingLatestLibrarySection>& sections) const;
    QCoro::Task<void> loadCachedPayloadAsync();
    QString payloadCacheKey() const;
    void saveCachedPayload(const QJsonObject& payload);
    QCoro::Task<void> refreshPlaybackRowsAsync(RequestGeneration::Token generation);
    void reconcilePlaybackRows(std::vector<MovieItem>& resume, std::vector<MovieItem>& nextUp);

    void updateProviderScope();
    void setHiddenProviderIds(QStringList moduleIds);
    void storeHiddenProviderIds(const QStringList& moduleIds);
    QCoro::Task<std::vector<MovieItem>> fetchResumeItems();
    QCoro::Task<std::vector<MovieItem>> fetchNextUpEpisodes();
    DatabaseManager *m_database = nullptr;
    Catalog *m_api = nullptr;
    LibraryPrefetchController *m_prefetch = nullptr;
    SourceHub *m_sources = nullptr;
    SettingsController *m_settings = nullptr;
    SourceHub::HomeQuery m_homeQuery;
    QStringList m_hiddenProviderIds;
    QString m_providerScopeMessage;
    QString m_providerAccountScopeKey;
    std::vector<LibraryItem> m_allLibraries;
    MovieGridModel m_resumeItems;
    MovieGridModel m_nextUpItems;
    std::vector<LatestLibrarySection> m_latestLibrarySections;
    RequestGeneration m_generation;
    RequestGeneration m_playbackRowsGeneration;
    RequestGeneration m_cacheGeneration;
    QSet<QString> m_locallyPlayed;
    QHash<QString, MovieItem> m_optimisticNextUp;
    bool m_refreshInFlight = false;
    bool m_loaded = false;
    bool m_networkResultsPublished = false;
    QStringList m_recentLibraryIds;
};

} // namespace Spool
