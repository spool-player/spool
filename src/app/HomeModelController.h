#pragma once

#include "../common/JellyfinTypes.h"
#include "../common/RequestGeneration.h"
#include "../models/MovieGridModel.h"
#include <QCoroTask>
#include <QJsonObject>

#include <QObject>
#include <QStringList>
#include <QVariantList>

#include <memory>
#include <vector>

namespace JellyfinNative {

class DatabaseManager;
class JellyfinApiFacade;
class LibraryPrefetchController;

class HomeModelController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(JellyfinNative::MovieGridModel *resumeItems READ resumeItems CONSTANT)
    Q_PROPERTY(JellyfinNative::MovieGridModel *nextUpItems READ nextUpItems CONSTANT)
    Q_PROPERTY(QVariantList latestLibraryRows READ latestLibraryRows NOTIFY latestLibraryRowsChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)

public:
    HomeModelController(DatabaseManager *database, JellyfinApiFacade *api, LibraryPrefetchController *prefetch,
        QObject *parent = nullptr);

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

    bool applyCachedPayload(const QJsonObject& payload);
    void loadCachedPayload();
    void refresh(const std::vector<LibraryItem>& libraries);
    void recordLibraryUse(const LibraryItem& library);
    void upsertResumeItem(MovieItem item, qint64 positionTicks);
    void updateResumeTicks(const QString& itemId, qint64 positionTicks);
    void updateFavorite(const QString& itemId, bool favorite);
    void updatePlayed(const QString& itemId, bool played);
    void reset();

signals:
    void latestLibraryRowsChanged();
    void loadingChanged();

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

    DatabaseManager *m_database = nullptr;
    JellyfinApiFacade *m_api = nullptr;
    LibraryPrefetchController *m_prefetch = nullptr;
    MovieGridModel m_resumeItems;
    MovieGridModel m_nextUpItems;
    std::vector<LatestLibrarySection> m_latestLibrarySections;
    RequestGeneration m_generation;
    bool m_refreshInFlight = false;
    bool m_loaded = false;
    QStringList m_recentLibraryIds;
};

} // namespace JellyfinNative
