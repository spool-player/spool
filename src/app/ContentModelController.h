#pragma once

#include "../common/RequestGeneration.h"
#include "../models/MovieGridModel.h"
#include "../provider/Catalog.h"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <vector>

namespace Spool {

class LibraryPrefetchController;

class ContentModelController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(Spool::MovieItem detailItem READ detailItem NOTIFY detailItemChanged)
    Q_PROPERTY(Spool::MovieGridModel *detailSeasons READ detailSeasons CONSTANT)
    Q_PROPERTY(Spool::MovieGridModel *detailSeasonOptions READ detailSeasonOptions CONSTANT)
    Q_PROPERTY(Spool::MovieGridModel *detailSimilarItems READ detailSimilarItems CONSTANT)
    Q_PROPERTY(Spool::MovieGridModel *linkedItems READ linkedItems CONSTANT)
    Q_PROPERTY(QVariantList personItemRows READ personItemRows NOTIFY personItemsChanged)
    Q_PROPERTY(bool detailRowsBusy READ detailRowsBusy NOTIFY detailRowsChanged)
    Q_PROPERTY(int detailContextInitialIndex READ detailContextInitialIndex NOTIFY detailRowsChanged)
    Q_PROPERTY(bool personItemsBusy READ personItemsBusy NOTIFY personItemsChanged)

public:
    ContentModelController(Catalog *catalog, LibraryPrefetchController *prefetch, QObject *parent = nullptr);

    MovieGridModel *detailSeasons()
    {
        return &m_detailSeasons;
    }
    MovieGridModel *detailSeasonOptions()
    {
        return &m_detailSeasonOptions;
    }
    MovieGridModel *detailSimilarItems()
    {
        return &m_detailSimilarItems;
    }
    QVariantList personItemRows() const;
    MovieGridModel *linkedItems()
    {
        return &m_linkedItems;
    }
    bool detailRowsBusy() const
    {
        return m_detailRowsBusy;
    }
    int detailContextInitialIndex() const
    {
        return m_detailContextInitialIndex;
    }
    bool personItemsBusy() const
    {
        return m_personItemsBusy;
    }
    MovieItem detailItem() const
    {
        return m_detailItem;
    }
    MovieItem detailSeasonAt(int index) const
    {
        return m_detailSeasons.movieAt(index);
    }

    Q_INVOKABLE void loadDetailRows(
        const QString& itemId, const QString& itemType, const QString& seriesId = {}, const QString& seasonId = {});
    Q_INVOKABLE void loadItemDetail(const QString& itemId);
    Q_INVOKABLE void loadPersonItems(const QString& personId);
    Q_INVOKABLE QVariantMap detailMediaInfo(const QString& preferredAudioLanguage) const;
    Q_INVOKABLE void prepareLinkedItem(const QString& itemId, const QString& title, const QString& itemType,
        const QString& seriesId = {}, const QString& seriesName = {}, const QString& seasonId = {});
    void updateResumeTicks(const QString& itemId, qint64 positionTicks);
    void updateFavorite(const QString& itemId, bool favorite);
    void updatePlayed(const QString& itemId, bool played);
    void reset();

signals:
    void detailRowsChanged();
    void detailItemChanged();
    void personItemsChanged();
    void errorOccurred(const QString& message);

private:
    struct PersonItemSection {
        QString title;
        QString kind;
        std::unique_ptr<MovieGridModel> model;
    };

    void finishDetailRowLoad(RequestGeneration::Token generation);
    void setPersonCredits(PersonCredits credits);
    void clearPersonItems();

    Catalog *m_api = nullptr;
    LibraryPrefetchController *m_prefetch = nullptr;
    MovieGridModel m_detailSeasons;
    MovieGridModel m_detailSeasonOptions;
    MovieGridModel m_detailSimilarItems;
    std::vector<PersonItemSection> m_personItemSections;
    MovieGridModel m_linkedItems;
    MovieItem m_detailItem;
    bool m_detailRowsBusy = false;
    int m_detailContextInitialIndex = 0;
    RequestGeneration m_detailRowsGeneration;
    RequestGeneration m_detailItemGeneration;
    int m_detailRowsPending = 0;
    bool m_personItemsBusy = false;
    RequestGeneration m_personItemsGeneration;
};

} // namespace Spool
