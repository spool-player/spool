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
    Q_PROPERTY(QString preferredProviderId READ preferredProviderId NOTIFY providerScopeChanged)
    Q_PROPERTY(QString providerId READ providerId NOTIFY providerScopeChanged)
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

    QString preferredProviderId() const
    {
        return m_preferredProviderId;
    }
    QString providerId() const
    {
        return m_homeQuery.moduleId;
    }
    QString providerScopeMessage() const
    {
        return m_providerScopeMessage;
    }
    QVariantList providerChoices() const;
    void attachSettings(SettingsController *settings);
    Q_INVOKABLE bool includesItem(const QString& scopedId) const;
    Q_INVOKABLE void selectProvider(const QString& moduleId);
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
    void setPreferredProviderId(const QString& moduleId);
    QCoro::Task<std::vector<MovieItem>> fetchResumeItems();
    QCoro::Task<std::vector<MovieItem>> fetchNextUpEpisodes();
    DatabaseManager *m_database = nullptr;
    Catalog *m_api = nullptr;
    LibraryPrefetchController *m_prefetch = nullptr;
    SourceHub *m_sources = nullptr;
    SettingsController *m_settings = nullptr;
    SourceHub::HomeQuery m_homeQuery;
    QString m_preferredProviderId;
    QString m_providerScopeMessage;
    std::vector<LibraryItem> m_allLibraries;
    MovieGridModel m_resumeItems;
    MovieGridModel m_nextUpItems;
    std::vector<LatestLibrarySection> m_latestLibrarySections;
    RequestGeneration m_generation;
    RequestGeneration m_playbackRowsGeneration;
    QSet<QString> m_locallyPlayed;
    QHash<QString, MovieItem> m_optimisticNextUp;
    bool m_refreshInFlight = false;
    bool m_loaded = false;
    QStringList m_recentLibraryIds;
};

} // namespace Spool
