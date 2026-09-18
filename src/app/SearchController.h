#pragma once

#include "../common/RequestGeneration.h"
#include "../models/MovieGridModel.h"
#include "../provider/SearchSource.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <vector>
namespace JellyfinNative {

class LibraryPrefetchController;

class SearchController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString query READ query NOTIFY queryChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(JellyfinNative::MovieGridModel *movieResults READ movieResults CONSTANT)
    Q_PROPERTY(JellyfinNative::MovieGridModel *seriesResults READ seriesResults CONSTANT)
    Q_PROPERTY(JellyfinNative::MovieGridModel *episodeResults READ episodeResults CONSTANT)
    Q_PROPERTY(JellyfinNative::MovieGridModel *otherResults READ otherResults CONSTANT)
    Q_PROPERTY(int resultCount READ resultCount NOTIFY resultsChanged)
    Q_PROPERTY(JellyfinNative::MovieGridModel *suggestions READ suggestions CONSTANT)
    Q_PROPERTY(bool suggestionsBusy READ suggestionsBusy NOTIFY suggestionsChanged)

public:
    SearchController(SearchSource *source, LibraryPrefetchController *prefetch, QObject *parent = nullptr);

    QString query() const
    {
        return m_query;
    }
    bool busy() const
    {
        return m_busy;
    }
    bool suggestionsBusy() const
    {
        return m_suggestionsBusy;
    }
    MovieGridModel *movieResults()
    {
        return &m_movieResults;
    }
    MovieGridModel *seriesResults()
    {
        return &m_seriesResults;
    }
    MovieGridModel *episodeResults()
    {
        return &m_episodeResults;
    }
    MovieGridModel *otherResults()
    {
        return &m_otherResults;
    }
    int resultCount() const
    {
        return m_movieResults.count() + m_seriesResults.count() + m_episodeResults.count() + m_otherResults.count();
    }
    MovieGridModel *suggestions()
    {
        return &m_suggestions;
    }

    Q_INVOKABLE void setQuery(const QString& query);
    Q_INVOKABLE void submit();
    Q_INVOKABLE void search(const QString& query);
    Q_INVOKABLE void clear();
    Q_INVOKABLE void loadSuggestions();

    void updateResumeTicks(const QString& itemId, qint64 positionTicks);
    void updateFavorite(const QString& itemId, bool favorite);
    void updatePlayed(const QString& itemId, bool played);
    void reset();

signals:
    void queryChanged();
    void busyChanged();
    void resultsChanged();
    void suggestionsChanged();
    void errorOccurred(const QString& message);

private:
    bool authenticated() const;
    void setBusy(bool busy);
    void setSuggestionsBusy(bool busy);

    void clearResults();
    void setResults(std::vector<MovieItem> items);
    SearchSource *m_api = nullptr;
    LibraryPrefetchController *m_prefetch = nullptr;
    MovieGridModel m_movieResults;
    MovieGridModel m_seriesResults;
    MovieGridModel m_episodeResults;
    MovieGridModel m_otherResults;
    MovieGridModel m_suggestions;
    QString m_query;
    bool m_busy = false;
    bool m_suggestionsBusy = false;
    bool m_suggestionsLoaded = false;
    RequestGeneration m_searchGeneration;
    RequestGeneration m_suggestionsGeneration;
    QTimer m_debounceTimer;
};

} // namespace JellyfinNative
