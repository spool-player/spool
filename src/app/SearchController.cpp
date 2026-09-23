#include "SearchController.h"

#include "../common/AsyncTask.h"
#include "LibraryPrefetchController.h"

#include <QDebug>
#include <QPointer>

#include <memory>
#include <utility>

namespace JellyfinNative {

namespace {
    constexpr int kSearchDebounceMs = 260;
}

SearchController::SearchController(SearchSource *source, LibraryPrefetchController *prefetch, QObject *parent)
    : QObject(parent)
    , m_api(source)
    , m_prefetch(prefetch)
{
    m_debounceTimer.setSingleShot(true);
    m_debounceTimer.setInterval(kSearchDebounceMs);
    connect(&m_debounceTimer, &QTimer::timeout, this, &SearchController::submit);
}

void SearchController::setQuery(const QString& query)
{
    const QString trimmed = query.trimmed();
    if (m_query != trimmed) {
        // A response arriving during debounce belongs to the old text, even
        // though the replacement request has not been submitted yet.
        m_searchGeneration.invalidate();
        m_query = trimmed;
        emit queryChanged();
    }

    if (m_query.size() < 2) {
        m_debounceTimer.stop();
        submit();
        return;
    }

    m_debounceTimer.start();
}

void SearchController::submit()
{
    m_debounceTimer.stop();

    if (m_query.size() < 2 || !authenticated()) {
        m_searchGeneration.invalidate();
        setBusy(false);
        clearResults();
        emit resultsChanged();
        return;
    }

    const RequestGeneration::Token generation = m_searchGeneration.next();
    setBusy(true);

    // Results show as each source answers; busy lasts until the last one.
    auto answered = std::make_shared<bool>(false);
    QPointer<SearchController> guard(this);
    auto update = [this, guard, generation, answered](std::vector<MovieItem> items) {
        if (!guard || !m_searchGeneration.isCurrent(generation))
            return;
        *answered = true;
        if (m_prefetch)
            m_prefetch->prefetchPosters(items);
        setResults(std::move(items));
        emit resultsChanged();
    };
    Async::runLatest(
        this, m_api->searchProgressively(m_query, 80, std::move(update)), m_searchGeneration, generation,
        [this, answered] {
            if (!*answered) {
                clearResults();
                emit resultsChanged();
            }
            setBusy(false);
        },
        [this](const std::exception_ptr& error) {
            clearResults();
            setBusy(false);
            emit resultsChanged();
            emit errorOccurred(exceptionMessage(error));
        });
}

void SearchController::search(const QString& query)
{
    const QString trimmed = query.trimmed();
    if (m_query != trimmed) {
        m_searchGeneration.invalidate();
        m_query = trimmed;
        emit queryChanged();
    }
    submit();
}

void SearchController::clear()
{
    m_debounceTimer.stop();
    m_searchGeneration.invalidate();
    if (!m_query.isEmpty()) {
        m_query.clear();
        emit queryChanged();
    }
    setBusy(false);
    clearResults();
    emit resultsChanged();
}

void SearchController::loadSuggestions()
{
    if (!authenticated())
        return;
    m_api->prepareSearch();
    if (m_suggestionsLoaded || m_suggestionsBusy)
        return;

    const RequestGeneration::Token generation = m_suggestionsGeneration.next();
    setSuggestionsBusy(true);

    Async::runLatest(
        this, m_api->fetchSearchSuggestions(), m_suggestionsGeneration, generation,
        [this](const std::vector<MovieItem>& items) {
            m_suggestions.setMovies(items);
            if (m_prefetch)
                m_prefetch->prefetchPosters(items);
            m_suggestionsLoaded = true;
            setSuggestionsBusy(false);
            emit suggestionsChanged();
        },
        [this](const std::exception_ptr& error) {
            m_suggestions.clear();
            setSuggestionsBusy(false);
            emit suggestionsChanged();
            qWarning() << "search: suggestions fetch failed" << exceptionMessage(error);
        });
}

void SearchController::updateResumeTicks(const QString& itemId, qint64 positionTicks)
{
    m_movieResults.updateResumeTicks(itemId, positionTicks);
    m_seriesResults.updateResumeTicks(itemId, positionTicks);
    m_episodeResults.updateResumeTicks(itemId, positionTicks);
    m_otherResults.updateResumeTicks(itemId, positionTicks);
    m_suggestions.updateResumeTicks(itemId, positionTicks);
}

void SearchController::updateFavorite(const QString& itemId, bool favorite)
{
    m_movieResults.updateFavorite(itemId, favorite);
    m_seriesResults.updateFavorite(itemId, favorite);
    m_episodeResults.updateFavorite(itemId, favorite);
    m_otherResults.updateFavorite(itemId, favorite);
    m_suggestions.updateFavorite(itemId, favorite);
}

void SearchController::updatePlayed(const QString& itemId, bool played)
{
    m_movieResults.updatePlayed(itemId, played);
    m_seriesResults.updatePlayed(itemId, played);
    m_episodeResults.updatePlayed(itemId, played);
    m_otherResults.updatePlayed(itemId, played);
    m_suggestions.updatePlayed(itemId, played);
}

void SearchController::reset()
{
    m_debounceTimer.stop();
    m_searchGeneration.invalidate();
    m_suggestionsGeneration.invalidate();
    clearResults();
    m_suggestions.clear();
    if (!m_query.isEmpty()) {
        m_query.clear();
        emit queryChanged();
    }
    setBusy(false);
    setSuggestionsBusy(false);
    m_suggestionsLoaded = false;
    emit resultsChanged();
    emit suggestionsChanged();
}

void SearchController::clearResults()
{
    m_movieResults.clear();
    m_seriesResults.clear();
    m_episodeResults.clear();
    m_otherResults.clear();
}

void SearchController::setResults(std::vector<MovieItem> items)
{
    std::vector<MovieItem> movies;
    std::vector<MovieItem> series;
    std::vector<MovieItem> episodes;
    std::vector<MovieItem> other;
    for (MovieItem& item : items) {
        if (item.itemType == QStringLiteral("Movie"))
            movies.push_back(std::move(item));
        else if (item.itemType == QStringLiteral("Series"))
            series.push_back(std::move(item));
        else if (item.itemType == QStringLiteral("Episode"))
            episodes.push_back(std::move(item));
        else
            other.push_back(std::move(item));
    }
    m_movieResults.setMovies(movies);
    m_seriesResults.setMovies(series);
    m_episodeResults.setMovies(episodes);
    m_otherResults.setMovies(other);
}

bool SearchController::authenticated() const
{
    return m_api && m_api->signedIn();
}

void SearchController::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

void SearchController::setSuggestionsBusy(bool busy)
{
    if (m_suggestionsBusy == busy)
        return;
    m_suggestionsBusy = busy;
    emit suggestionsChanged();
}

} // namespace JellyfinNative
