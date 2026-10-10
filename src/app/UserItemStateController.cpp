#include "UserItemStateController.h"

#include "../common/AsyncTask.h"

#include "BrowseSessionController.h"
#include "ContentModelController.h"
#include "HomeModelController.h"
#include "SearchController.h"

namespace Spool {

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

    if (m_content)
        m_content->updateResumeTicks(itemId, positionTicks);
    if (m_browse)
        m_browse->updateResumeTicks(itemId, positionTicks);
    if (m_home)
        m_home->updateResumeTicks(itemId, positionTicks);
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

    if (m_content)
        m_content->updatePlayed(itemId, played);
    if (m_browse)
        m_browse->updatePlayed(itemId, played);
    if (m_home)
        m_home->updatePlayed(itemId, played);
    if (m_search)
        m_search->updatePlayed(itemId, played);
    emit playedChanged(itemId, played);
}

void UserItemStateController::recordPlaybackStopped(const MovieItem& item, const QString& itemId, qint64 positionTicks,
    bool watched, const MovieItem& successor, quint64 reportId)
{
    // An older manual write may still finish, but its acknowledgement must
    // not erase progress or completion observed after the user requested it.
    const auto pending = m_pendingWrites.find(itemId);
    if (pending != m_pendingWrites.end())
        pending->playbackRevision.invalidate();
    if (!watched) {
        applyResumeTicks(itemId, positionTicks);
        if (m_home && item.id == itemId)
            m_home->upsertResumeItem(item, positionTicks);
        return;
    }
    if (itemId.isEmpty())
        return;
    if (reportId != 0)
        m_pendingPlaybackWatched.insert(itemId, reportId);
    if (m_home && item.id == itemId)
        m_home->advanceNextUp(item, successor);
    applyPlayed(itemId, true);
}

void UserItemStateController::persistPlaybackWatched(const QString& itemId, quint64 reportId)
{
    const auto pending = m_pendingPlaybackWatched.constFind(itemId);
    if (pending == m_pendingPlaybackWatched.cend() || pending.value() != reportId)
        return;
    m_pendingPlaybackWatched.remove(itemId);
    // Models already reflect the observed completion; only the server write
    // remains, serialized behind any earlier manual writes for this item.
    enqueueWrite(itemId, WriteKind::Played, true, false);
}

void UserItemStateController::reset()
{
    m_writeGeneration.invalidate();
    m_pendingWrites.clear();
    m_pendingPlaybackWatched.clear();
}

void UserItemStateController::setFavorite(const QString& itemId, bool favorite)
{
    enqueueWrite(itemId, WriteKind::Favorite, favorite);
}

void UserItemStateController::setPlayed(const QString& itemId, bool played)
{
    m_pendingPlaybackWatched.remove(itemId);
    enqueueWrite(itemId, WriteKind::Played, played);
}

void UserItemStateController::clearProgress(const QString& itemId)
{
    m_pendingPlaybackWatched.remove(itemId);
    enqueueWrite(itemId, WriteKind::ClearProgress, false);
}

void UserItemStateController::enqueueWrite(const QString& itemId, WriteKind kind, bool value, bool publish)
{
    if (itemId.isEmpty() || !m_api || !m_api->signedIn())
        return;

    // Serialize writes for an item so the server sees the same order as the
    // client. Manual edits become visible only after the server accepts them;
    // a failed edit therefore cannot lose resume rows or invent a prior value.
    auto& pending = m_pendingWrites[itemId];
    pending.requests.enqueue({ kind, value, publish, pending.playbackRevision.current() });
    if (pending.requests.size() == 1)
        startNextWrite(itemId);
}

void UserItemStateController::startNextWrite(const QString& itemId)
{
    const auto pending = m_pendingWrites.constFind(itemId);
    if (pending == m_pendingWrites.cend() || pending->requests.isEmpty())
        return;
    const PendingWrite write = pending->requests.head();
    const auto generation = m_writeGeneration.current();
    QCoro::Task<void> task = write.kind == WriteKind::Favorite ? m_api->setItemFavorite(itemId, write.value)
        : write.kind == WriteKind::Played                      ? m_api->setItemPlayed(itemId, write.value)
                                                               : m_api->setItemPlaybackPosition(itemId, 0);
    Async::runLatest(
        this, std::move(task), m_writeGeneration, generation,
        [this, itemId, write, generation]() { finishWrite(itemId, write, generation); },
        [this, itemId, write, generation](
            const std::exception_ptr& error) { finishWrite(itemId, write, generation, error); },
        "item state write");
}

void UserItemStateController::finishWrite(const QString& itemId, const PendingWrite& write,
    RequestGeneration::Token generation, const std::exception_ptr& error)
{
    auto pending = m_pendingWrites.find(itemId);
    if (pending == m_pendingWrites.end())
        return;
    const bool publish = write.publish
        && (write.kind == WriteKind::Favorite || pending->playbackRevision.isCurrent(write.playbackRevision));
    pending->requests.dequeue();
    const bool more = !pending->requests.isEmpty();
    if (!more)
        m_pendingWrites.erase(pending);

    QPointer<UserItemStateController> guard(this);
    if (error) {
        emit errorOccurred(exceptionMessage(error));
    } else if (publish) {
        if (write.kind == WriteKind::Favorite) {
            applyFavorite(itemId, write.value);
        } else if (write.kind == WriteKind::ClearProgress) {
            applyResumeTicks(itemId, 0);
            applyPlayed(itemId, false);
        } else {
            applyPlayed(itemId, write.value);
        }
    }
    if (!error && m_home && write.kind != WriteKind::Favorite)
        m_home->refreshPlaybackRows();
    // Model and error signals can reset the account or destroy the controller.
    if (guard && m_writeGeneration.isCurrent(generation) && more)
        startNextWrite(itemId);
}

} // namespace Spool
