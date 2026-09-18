#include "UserItemStateController.h"

#include "../common/AsyncTask.h"

#include "BrowseSessionController.h"
#include "ContentModelController.h"
#include "HomeModelController.h"
#include "SearchController.h"

namespace JellyfinNative {

UserItemStateController::UserItemStateController(UserItemStateSink *sink, BrowseSessionController *currentItems,
    HomeModelController *home, ContentModelController *content, SearchController *search, QObject *parent)
    : QObject(parent)
    , m_api(sink)
    , m_browse(currentItems)
    , m_home(home)
    , m_content(content)
    , m_search(search)
{
}

void UserItemStateController::applyResumeTicks(const QString& itemId, qint64 positionTicks)
{
    if (itemId.isEmpty() || positionTicks < 0)
        return;

    if (m_browse)
        m_browse->updateResumeTicks(itemId, positionTicks);
    if (m_home)
        m_home->updateResumeTicks(itemId, positionTicks);
    if (m_content)
        m_content->updateResumeTicks(itemId, positionTicks);
    if (m_search)
        m_search->updateResumeTicks(itemId, positionTicks);
}

void UserItemStateController::applyFavorite(const QString& itemId, bool favorite)
{
    if (itemId.isEmpty())
        return;

    if (m_browse)
        m_browse->updateFavorite(itemId, favorite);
    if (m_home)
        m_home->updateFavorite(itemId, favorite);
    if (m_content)
        m_content->updateFavorite(itemId, favorite);
    if (m_search)
        m_search->updateFavorite(itemId, favorite);
    emit favoriteChanged(itemId, favorite);
}

void UserItemStateController::applyPlayed(const QString& itemId, bool played)
{
    if (itemId.isEmpty())
        return;

    if (m_browse)
        m_browse->updatePlayed(itemId, played);
    if (m_home)
        m_home->updatePlayed(itemId, played);
    if (m_content)
        m_content->updatePlayed(itemId, played);
    if (m_search)
        m_search->updatePlayed(itemId, played);
    emit playedChanged(itemId, played);
}

void UserItemStateController::recordPlaybackStopped(
    const MovieItem& item, const QString& itemId, qint64 positionTicks, bool completed)
{
    if (!completed) {
        applyResumeTicks(itemId, positionTicks);
        if (m_home && item.id == itemId)
            m_home->upsertResumeItem(item, positionTicks);
        return;
    }
    applyPlayed(itemId, true);
    if (!m_api || !m_api->signedIn())
        return;
    Async::runScoped(
        this, m_api->setItemPlayed(itemId, true), []() {},
        [this](const std::exception_ptr& error) { emit errorOccurred(exceptionMessage(error)); });
}

void UserItemStateController::setFavorite(const QString& itemId, bool favorite)
{
    if (itemId.isEmpty() || !m_api || !m_api->signedIn())
        return;
    applyFavorite(itemId, favorite);
    Async::runScoped(
        this, m_api->setItemFavorite(itemId, favorite), []() {},
        [this, itemId, favorite](const std::exception_ptr& error) {
            applyFavorite(itemId, !favorite);
            emit errorOccurred(exceptionMessage(error));
        });
}

void UserItemStateController::setPlayed(const QString& itemId, bool played)
{
    if (itemId.isEmpty() || !m_api || !m_api->signedIn())
        return;
    applyPlayed(itemId, played);
    Async::runScoped(
        this, m_api->setItemPlayed(itemId, played), []() {},
        [this, itemId, played](const std::exception_ptr& error) {
            applyPlayed(itemId, !played);
            emit errorOccurred(exceptionMessage(error));
        });
}

void UserItemStateController::clearProgress(const QString& itemId)
{
    if (itemId.isEmpty() || !m_api || !m_api->signedIn())
        return;
    applyResumeTicks(itemId, 0);
    applyPlayed(itemId, false);
    Async::runScoped(
        this, m_api->setItemPlaybackPosition(itemId, 0), []() {},
        [this](const std::exception_ptr& error) { emit errorOccurred(exceptionMessage(error)); });
}

} // namespace JellyfinNative
