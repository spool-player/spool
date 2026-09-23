#include "AppController.h"

#include "ArtworkService.h"

#include "../common/AsyncTask.h"
#include "../common/SeriesAudioSelection.h"
#include "../diagnostics/Diagnostics.h"
#include "../platform/PlatformPaths.h"
#include "../player/PlayQueueController.h"
#include "../player/PlaybackFailurePolicy.h"
#include "../player/PlayerController.h"
#include "../provider/Catalog.h"
#include "../provider/GroupPlayback.h"
#include "../provider/PlaybackSource.h"
#include "../provider/SourceHub.h"
#include "../provider/StreamQualityControl.h"
#include "BrowseSessionController.h"
#include "ContentModelController.h"
#include "GroupPlaybackController.h"
#include "HomeModelController.h"
#include "LibraryPrefetchController.h"
#include "LibraryQuery.h"
#include "SearchController.h"
#include "SettingsController.h"
#include "UserItemStateController.h"

#include <QDebug>
#include <QPixmapCache>
#include <QStringList>
#include <QTimer>
#include <QUuid>
#include <QVariantMap>
#include <QWindow>

#include <algorithm>
#include <memory>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace Spool {

namespace {

    constexpr int kLibraryPageSize = 100;
    // Long enough to cover a slow negotiate, short enough that a stuck one does
    // not leave the shell holding a player surface over nothing.
    constexpr int kPlaybackTransitionTimeoutMs = 12000;
    // Reopening a library within this window shows the cached page as-is;
    // a server refresh so soon after the last one only causes delegate churn.
    constexpr qint64 kFreshLibraryCacheMs = 30000;
    // v3: keyed by the catalog's scope rather than a Jellyfin server and user.
    constexpr auto kSeriesTrackSelectionNamespace = "series-track-selection-v3";
    constexpr auto kRememberSeriesAudioTrackKey = "playback/rememberSeriesAudioTrack";

    QString seriesTrackSelectionKey(const QString& libraryScopeKey, const QString& seriesId)
    {
        return libraryScopeKey + QLatin1Char('/') + seriesId;
    }

    QByteArray encodeTrackSelection(const SeriesAudioPreference& audioPreference, int subtitleStreamIndex)
    {
        return audioPreference.language.toUtf8() + ',' + QByteArray::number(audioPreference.languageTrackNumber) + ','
            + QByteArray::number(subtitleStreamIndex);
    }

    bool decodeTrackSelection(const QByteArray& value, SeriesAudioPreference& audioPreference, int& subtitleStreamIndex)
    {
        const QList<QByteArray> parts = value.split(',');
        if (parts.size() != 3)
            return false;
        bool trackNumberOk = false;
        bool subtitleIndexOk = false;
        const int languageTrackNumber = parts.at(1).toInt(&trackNumberOk);
        const int storedSubtitleStreamIndex = parts.at(2).toInt(&subtitleIndexOk);
        if (!trackNumberOk || !subtitleIndexOk)
            return false;
        audioPreference = { QString::fromUtf8(parts.at(0)).trimmed(), languageTrackNumber };
        subtitleStreamIndex = storedSubtitleStreamIndex;
        return true;
    }

    bool isBrowseContainer(const MovieItem& item)
    {
        return item.itemType == QStringLiteral("Playlist") || item.itemType == QStringLiteral("BoxSet")
            || item.itemType == QStringLiteral("Folder") || item.itemType == QStringLiteral("PhotoAlbum")
            || item.itemType == QStringLiteral("MusicAlbum") || item.itemType == QStringLiteral("MusicArtist");
    }

}

AppController::AppController(
    DatabaseManager *database, SourceHub *provider, ArtworkService *artwork, PlayerController *player, QObject *parent)
    : QObject(parent)
    , m_database(database)
    , m_provider(provider)
    , m_catalog(provider->catalog())
    , m_playback(provider->playback())
    , m_quality(provider->streamQuality())
    , m_artwork(artwork)
    , m_player(player)
{
    m_playQueue = new PlayQueueController(m_playback, this);
    m_settings = new SettingsController(database, player, artwork, this);
    m_prefetch = new LibraryPrefetchController(m_catalog, artwork, this);
    m_browse = new BrowseSessionController(m_prefetch, this);
    m_home = new HomeModelController(database, m_catalog, m_prefetch, this);
    m_content = new ContentModelController(m_catalog, m_prefetch, this);
    m_search = new SearchController(provider->search(), m_prefetch, this);
    m_itemState = new UserItemStateController(provider->itemState(), m_browse, m_home, m_content, m_search, this);
    m_group = new GroupPlaybackController(provider, player, m_playQueue, this);
    connect(m_group, &GroupPlaybackController::errorText, this, &AppController::showToast);
    connect(provider, &SourceHub::accountEvent, this,
        [this](const QString& accountId, const QString& type, const QVariantMap& payload) {
            if (type == QStringLiteral("remote"))
                handleRemoteCommand(accountId, payload);
        });
    connect(m_playQueue, &PlayQueueController::successorPlaybackReady, this, [this]() { playQueueCurrent(false); });
    connect(m_browse, &BrowseSessionController::reloadRequested, this, [this]() { beginBrowse(); });
    connect(m_browse, &BrowseSessionController::moreItemsRequested, this, &AppController::loadMoreCurrentItems);
    connect(m_database, &DatabaseManager::recoveryNotice, this, &AppController::showToast);
    {
        connect(m_group, &GroupPlayback::queuePlaybackRequested, this, [this](qint64 positionTicks) {
            MovieItem item = m_playQueue->currentItem();
            if (item.id.isEmpty())
                return;
            item.resumeTicks = std::max<qint64>(0, positionTicks);
            if (m_player->sessionActive() && m_activePlaybackItem.id != item.id)
                m_player->stopWithReason(QStringLiteral("syncplay-group-switch"));
            m_activePlaybackItem = item;
            setBusy(true, QStringLiteral("Joining the group…"));
            Async::runScoped(
                this, startPlayback(item, true), []() {},
                [this](const std::exception_ptr& error) {
                    setBusy(false);
                    showToast(exceptionMessage(error));
                },
                "group playback startup");
        });
    }
    connect(m_content, &ContentModelController::errorOccurred, this, &AppController::showToast);
    connect(m_search, &SearchController::errorOccurred, this, &AppController::showToast);
    connect(m_itemState, &UserItemStateController::errorOccurred, this, &AppController::setErrorText);
    connect(m_settings, &SettingsController::errorOccurred, this, &AppController::showToast);
    connect(m_provider, &Provider::toastRequested, this, &AppController::showToast);
    connect(m_provider, &Provider::errorOccurred, this, &AppController::setErrorText);
    connect(m_provider, &Provider::contentChanged, this, [this](const QString& changedItemId) {
        if (!changedItemId.isEmpty() && m_browse->descriptor().id == changedItemId) {
            goHome();
            return;
        }
        // No item: an account came or went, or a whole library changed, so
        // the library list and home rows are rebuilt too.
        if (changedItemId.isEmpty())
            loadLibraries();
        beginBrowse();
    });
    connect(m_provider, &Provider::sessionStarted, this, [this]() {
        m_home->loadCachedPayload();
        loadLibraries();
    });
    connect(m_provider, &Provider::sessionEnded, this, &AppController::resetApplicationState);

    connect(m_player, &PlayerController::playbackStopped, this, &AppController::handlePlaybackStopped);
    // The device has just shown it cannot sustain the picture it was asked
    // for. Move down one rung and say so plainly; the setting persists, so
    // the next thing that plays starts where this one ended up rather than
    // dropping frames again on the way to the same conclusion.
    connect(m_player, &PlayerController::renderQualityStrained, this, [this](qint64 droppedFrames) {
        if (!m_settings || !m_player->sessionActive() || !m_player->fileLoaded() || m_busy)
            return;
        const QString lowered = m_settings->stepDownRenderQuality();
        if (lowered.isEmpty())
            return;
        qInfo() << "player: lowering picture quality to" << lowered << "after" << droppedFrames << "dropped frames";
        showToast(QStringLiteral("Switched to faster playback for this device."));
        const qint64 positionTicks = static_cast<qint64>(m_player->positionSeconds() * 10'000'000.0);
        const MovieItem resumeItem = PlaybackFailurePolicy::retryItem(m_activePlaybackItem, positionTicks);
        setBusy(true, QStringLiteral("Switching to faster playback…"));
        Async::runScoped(
            this,
            startPlayback(
                resumeItem, m_player->paused(), false, m_activeAudioStreamIndex, m_activeSubtitleStreamIndex, true),
            []() {},
            [this](const std::exception_ptr& error) {
                setBusy(false);
                showToast(exceptionMessage(error));
            },
            "render quality fallback");
    });
    connect(m_player, &PlayerController::playbackStateChanged, this, [this]() {
        if (m_player->fileLoaded())
            m_qualityFallbackBitrate = -1;
    });
    // Keep the bandwidth probe off the wire while a stream is running; it
    // resumes on its own once the session ends.
    connect(m_player, &PlayerController::visibleChanged, this, [this]() {
        if (m_player->visible())
            setPlaybackTransition(false);
    });
    connect(m_player, &PlayerController::streamSelectionChanged, this,
        [this](int audioStreamIndex, int subtitleStreamIndex) {
            // Kept so a quality change can renegotiate onto the same tracks.
            m_activeAudioStreamIndex = audioStreamIndex;
            m_activeSubtitleStreamIndex = subtitleStreamIndex;
            if (m_activePlaybackItem.itemType != QStringLiteral("Episode") || m_activePlaybackItem.seriesId.isEmpty())
                return;
            const bool rememberAudio = m_settings->value(QString::fromLatin1(kRememberSeriesAudioTrackKey)).toBool();
            const SeriesAudioPreference audioPreference = rememberAudio
                ? seriesAudioPreferenceForSelection(m_activePlaybackStreams, audioStreamIndex)
                : SeriesAudioPreference {};
            m_database->saveCacheEntry(QString::fromLatin1(kSeriesTrackSelectionNamespace),
                seriesTrackSelectionKey(m_catalog->libraryScopeKey(), m_activePlaybackItem.seriesId),
                encodeTrackSelection(audioPreference, subtitleStreamIndex));
        });
    connect(m_player, &PlayerController::playbackLoadFailed, this,
        [this](const QString& itemId, qint64 positionTicks, const QString&, bool retryableCodecFailure,
            int audioStreamIndex, int subtitleStreamIndex) {
            const bool syncPlayActive = inGroup();
            if (itemId.isEmpty() || itemId != m_activePlaybackItem.id)
                return;
            // A quality the server cannot deliver should cost the viewer the
            // quality, not the thing they were watching.
            if (m_qualityFallbackBitrate >= 0) {
                const qint64 restoredBitrate = m_qualityFallbackBitrate;
                const int restoredHeight = m_qualityFallbackHeight;
                m_qualityFallbackBitrate = -1;
                m_qualityFallbackHeight = 0;
                if (m_quality)
                    m_quality->setOverride(restoredBitrate, restoredHeight);
                else {
                    m_genericBitrateOverride = restoredBitrate;
                    m_genericHeightOverride = restoredHeight;
                }
                emit streamingQualityChanged();
                const MovieItem resumeItem = PlaybackFailurePolicy::retryItem(m_activePlaybackItem, positionTicks);
                setBusy(true, QStringLiteral("Restoring the previous quality…"));
                showToast(QStringLiteral("That quality could not be played; keeping the previous one."));
                Async::runScoped(
                    this, startPlayback(resumeItem, false, false, audioStreamIndex, subtitleStreamIndex), []() {},
                    [this](const std::exception_ptr& error) {
                        setBusy(false);
                        showToast(exceptionMessage(error));
                    },
                    "playback quality restore");
                return;
            }
            if (retryableCodecFailure && syncPlayActive) {
                m_group->leaveGroup();
                showToast(QStringLiteral("This stream is not directly compatible. Leaving SyncPlay; start it again to "
                                         "use server transcoding."));
                return;
            }
            if (!PlaybackFailurePolicy::shouldStartCodecFallback(
                    retryableCodecFailure, m_codecFallbackAttempted, syncPlayActive)) {
                return;
            }
            m_codecFallbackAttempted = true;
            const MovieItem retryItem = PlaybackFailurePolicy::retryItem(m_activePlaybackItem, positionTicks);
            m_player->teardownMpv();
            setBusy(true, QStringLiteral("Trying a compatible server stream…"));
            showToast(QStringLiteral("Direct playback was not supported; retrying once with server transcoding."));
            Async::runScoped(
                this, startPlayback(retryItem, false, true, audioStreamIndex, subtitleStreamIndex), []() {},
                [this](const std::exception_ptr& error) {
                    setBusy(false);
                    showToast(exceptionMessage(error));
                },
                "playback codec fallback");
        });
}

int AppController::claimInstanceSlot()
{
    // QLockFile only treats a lock as stale once its owner is gone, so the
    // slot a running instance holds stays taken. A read-only data root leaves
    // every attempt failing, which lands on the stored identity as before.
    constexpr int kMaxLocalInstances = 8;
    const QString dataRoot = persistentDataRoot();
    for (int slot = 0; slot < kMaxLocalInstances; ++slot) {
        auto lock = std::make_unique<QLockFile>(QStringLiteral("%1/instance-%2.lock").arg(dataRoot).arg(slot));
        if (lock->tryLock(0)) {
            m_instanceLock = std::move(lock);
            return slot;
        }
    }
    qWarning() << "app: no free instance slot; sharing the stored device identity";
    return 0;
}

void AppController::initialize()
{
    Async::runScoped(
        this, initializeAsync(), []() {},
        [this](const std::exception_ptr& error) { setErrorText(exceptionMessage(error)); }, "app initialize");
}

QCoro::Task<void> AppController::initializeAsync()
{
    Diagnostics::Task task(QStringLiteral("app_initialize"));
    StartupState startupState = co_await m_database->loadStartupStateAsync(SettingsController::localSettingKeys());

    QString deviceId = std::move(startupState.deviceId);
    if (deviceId.isEmpty()) {
        deviceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_database->saveDeviceId(deviceId);
    }
    // Servers key a session on the device id, so two instances sharing one
    // collapse into a single session and receive each other's socket
    // messages. Every extra instance gets its own, derived from the stored
    // one so the first is unchanged.
    const int instanceSlot = claimInstanceSlot();
    if (instanceSlot > 0) {
        deviceId += QStringLiteral("-%1").arg(instanceSlot + 1);
        qInfo() << "app: another instance holds the stored device identity; running as instance" << instanceSlot + 1;
    }
    m_settings->applyLocalValues(startupState.values);
    emit deviceIdentityReady(deviceId);

    m_initialized = true;
    emit initializedChanged();
}

void AppController::resetApplicationState()
{
    m_remotePlaybackRequestGeneration.invalidate();
    m_prefetch->stop();
    if (m_artwork)
        m_artwork->cancelPrefetches();
    if (m_database)
        m_database->invalidateHomePayloads();
    m_libraries.clear();
    m_browse->clear();
    m_home->reset();
    m_content->reset();
    m_search->reset();
    m_activePlaybackItem = {};
    m_libraryLoadGeneration.invalidate();
    m_browse->reset();
    setBusy(false);
    setErrorText({});
}

void AppController::goHome()
{
    if (!m_provider->ready())
        return;

    qInfo() << "app: go home viewKind=" << m_browse->viewKind();
    m_libraryLoadGeneration.invalidate();
    setBusy(false);
    refreshHomeRows();
}

void AppController::openLibrary(int index)
{
    const LibraryItem library = m_libraries.libraryAt(index);
    if (library.id.isEmpty())
        return;
    const QVariantMap defaultQuery = defaultLibraryQuery(library);
    m_browse->enterLibrary(library, defaultQuery);
    m_home->recordLibraryUse(library);
    loadLibraryFilterOptions(beginBrowse(m_browse->query() == defaultQuery), library);
}

bool AppController::openLibraryById(const QString& libraryId)
{
    if (libraryId.isEmpty())
        return false;
    for (int index = 0; index < m_libraries.count(); ++index) {
        if (m_libraries.libraryAt(index).id == libraryId) {
            openLibrary(index);
            return true;
        }
    }
    return false;
}

void AppController::playOrOpen(const MovieItem& item, bool fromStart)
{
    if (item.id.isEmpty())
        return;
    if (m_browse->enterItem(item)) {
        beginBrowse();
    } else {
        playQueuedItem(item, fromStart);
    }
}
void AppController::playItemId(const QString& itemId, bool fromStart)
{
    if (itemId.isEmpty() || !m_catalog)
        return;
    setBusy(true, QStringLiteral("Loading item for playback…"));
    Async::runScoped(
        this, m_catalog->fetchItemDetails(itemId),
        [this, fromStart](const MovieItem& item) {
            setBusy(false);
            if (!item.id.isEmpty())
                playQueuedItem(item, fromStart);
            else
                showToast(QStringLiteral("Item not found."));
        },
        [this](const std::exception_ptr& error) {
            setBusy(false);
            showToast(exceptionMessage(error));
        },
        "play item by id");
}

void AppController::playFromModel(QObject *model, int index, bool fromStart)
{
    if (!model)
        return;

    if (auto *queue = qobject_cast<PlayQueueController *>(model)) {
        if (queue != m_playQueue) {
            showToast(QStringLiteral("This item is no longer in the play queue."));
            return;
        }
        if (!queue->playAt(index)) {
            showToast(QStringLiteral("This item is no longer in the play queue."));
            return;
        }
        startQueuedPlayback(fromStart);
        return;
    }

    auto *movieModel = qobject_cast<MovieGridModel *>(model);
    if (!movieModel) {
        showToast(QStringLiteral("This item cannot be played from the current list."));
        return;
    }
    const MovieItem item = movieModel->movieAt(index);
    if (isBrowseContainer(item))
        playOrOpen(item, fromStart);
    else if (item.itemType == QStringLiteral("Episode") && !item.seriesId.isEmpty())
        playQueuedItem(item, fromStart);
    else if (item.itemType == QStringLiteral("Audio") && !item.albumId.isEmpty() && !modelIsOrderedList(movieModel))
        playAlbumFrom(item, fromStart);
    else if (modelIsOrderedList(movieModel))
        playQueuedItems(movieModel->movies(), index, fromStart);
    else
        playQueuedItem(item, fromStart);
}

// Whether the list a track was picked out of is one the user assembled or
// opened on purpose, or just a shelf it happened to appear on. Playing a song
// off the home screen should queue its album; playing one out of a playlist
// should keep the playlist. Deciding by model identity keeps the rule here
// rather than spreading a flag across every call site in QML.
bool AppController::modelIsOrderedList(MovieGridModel *model) const
{
    if (!model)
        return false;
    if (model == m_content->detailSeasons())
        return true;
    if (model != m_browse->items())
        return false;
    switch (m_browse->descriptor().kind) {
    case BrowseKind::FolderChildren:
    case BrowseKind::Playlist:
    case BrowseKind::BoxSet:
    case BrowseKind::SeasonEpisodes:
    case BrowseKind::ArtistAlbums:
        return true;
    default:
        return false;
    }
}

void AppController::playAlbumFrom(const MovieItem& track, bool fromStart)
{
    if (track.albumId.isEmpty()) {
        playQueuedItem(track, fromStart);
        return;
    }

    const quint64 generation = ++m_albumQueueGeneration;
    setBusy(true, QStringLiteral("Loading album…"));
    // The same descriptor ContentModelController uses for album children, so
    // there is one definition of what an album contains.
    Async::runScoped(
        this, m_catalog->fetchBrowsePage(BrowseDescriptor::folderChildren(track.albumId), 0, 200, {}),
        [this, generation, track, fromStart](const PagedMovieItems& page) {
            if (generation != m_albumQueueGeneration)
                return;
            setBusy(false);
            const auto found = std::find_if(page.items.cbegin(), page.items.cend(),
                [&track](const MovieItem& candidate) { return candidate.id == track.id; });
            if (found == page.items.cend()) {
                playQueuedItem(track, fromStart);
                return;
            }
            playQueuedItems(page.items, static_cast<int>(std::distance(page.items.cbegin(), found)), fromStart);
        },
        [this, generation, track, fromStart](const std::exception_ptr&) {
            if (generation != m_albumQueueGeneration)
                return;
            setBusy(false);
            // The track is still what the user asked for; losing the rest of
            // the album is a worse outcome than not playing at all.
            playQueuedItem(track, fromStart);
        });
}

void AppController::stopPlayback()
{
    // A system stop can arrive between tracks, while no mpv session exists.
    // Invalidate negotiation as well so its reply cannot restart stopped music.
    m_playbackLoadGeneration.invalidate();
    setPlaybackTransition(false);
    setBusy(false);
    m_player->stop();
}

void AppController::playQueueNext()
{
    if (!m_playQueue->canGoNext()) {
        playEpisodeWithContext(m_playQueue->currentItem(), 1, true);
        return;
    }
    if (inGroup()) {
        m_group->requestNextItem();
        return;
    }
    if (!m_playQueue->next())
        return;
    playQueueCurrent(false);
}

void AppController::playQueuePrevious()
{
    if (!m_playQueue->canGoPrevious()) {
        playEpisodeWithContext(m_playQueue->currentItem(), -1, true);
        return;
    }
    if (inGroup()) {
        m_group->requestPreviousItem();
        return;
    }
    if (!m_playQueue->previous())
        return;
    playQueueCurrent(true);
}

void AppController::playQueueItem(int index)
{
    // A second click on the row already playing used to tear mpv down and
    // restart the same track from the top, which is never what the click meant.
    if (index == m_playQueue->currentIndex() && m_player->sessionActive())
        return;

    if (inGroup()) {
        // Jumping the group, not just this client.
        m_group->requestPlayItem(queueEntryId(index));
        return;
    }
    if (!m_playQueue->playAt(index))
        return;
    playQueueCurrent(false);
}

// Previewed locally even in a group. Waiting on a round trip per step would
// make a held D-pad key and a pointer drag both unusable; the group's own
// PlayQueue broadcast is what settles the order a moment later.
bool AppController::previewQueueMoveRange(int from, int count, int to)
{
    return m_playQueue->moveRange(from, count, to);
}

void AppController::commitQueueMoveRange(int from, int count, int to)
{
    if (from == to || count <= 0 || !inGroup())
        return;
    // The preview has already laid the block down at `to`, so publish the rows
    // where they now sit, top down, which is the order the group will apply.
    for (int offset = 0; offset < count; ++offset)
        m_group->requestMoveItem(queueEntryId(to + offset), to + offset);
}

void AppController::removeQueueItem(int index)
{
    if (inGroup()) {
        // No local edit: a removal is a single action with nothing to animate,
        // so let the group's broadcast be the one thing that changes the queue.
        m_group->requestRemoveItems({ queueEntryId(index) });
        return;
    }

    const bool removingCurrent = index == m_playQueue->currentIndex();
    if (!m_playQueue->removeItem(index))
        return;
    if (!removingCurrent)
        return;

    // The model hands the cursor to whatever followed the removed row, but it
    // has no player to act on it — the row that was playing is gone, so
    // something has to take its place or stop.
    if (m_playQueue->currentIndex() < 0) {
        m_player->stopWithReason(QStringLiteral("queue-cleared"));
        return;
    }
    playQueueCurrent(false);
}

void AppController::playNextFromItem(const MovieItem& item)
{
    if (enqueueForGroup(item, true))
        return;
    if (!m_playQueue->playNext(item))
        setErrorText(QStringLiteral("This item cannot be queued."));
}

void AppController::addToQueueFromItem(const MovieItem& item)
{
    if (enqueueForGroup(item, false))
        return;
    if (!m_playQueue->addToQueue(item))
        setErrorText(QStringLiteral("This item cannot be queued."));
}

void AppController::loadMoreCurrentItems()
{
    if (m_browse->loadingMore() || !m_browse->hasMore())
        return;
    if (!m_catalog->signedIn())
        return;
    const BrowseDescriptor descriptor = m_browse->descriptor();
    if (!descriptor.isValid())
        return;

    const int startIndex = std::max(m_browse->nextStartIndex(), m_browse->rowCount());
    const RequestGeneration::Token loadGeneration = m_libraryLoadGeneration.current();
    const QString cacheKey = m_browse->cacheKey();
    const QVariantMap query = descriptor.kind == BrowseKind::Library ? m_browse->query() : QVariantMap {};
    if (m_artwork)
        m_artwork->cancelPrefetches();
    m_browse->setLoadingMore(true);

    const auto onDone = [this, cacheKey](const PagedMovieItems& page) { showCurrentItemsPage(page, cacheKey, true); };
    const auto onError = [this](const std::exception_ptr& error) {
        m_browse->setLoadingMore(false);
        showToast(exceptionMessage(error));
    };

    Async::runLatest(this, m_catalog->fetchBrowsePage(descriptor, startIndex, kLibraryPageSize, query),
        m_libraryLoadGeneration, loadGeneration, onDone, onError);
}

void AppController::playQueuedItems(const std::vector<MovieItem>& items, int startIndex, bool fromStart)
{
    if (!m_playQueue->playNow(items, startIndex)) {
        showToast(QStringLiteral("This item cannot be queued."));
        return;
    }
    startQueuedPlayback(fromStart);
}

void AppController::playModel(MovieGridModel *model, bool shuffled)
{
    if (!model)
        return;
    const std::vector<MovieItem>& items = model->movies();
    const auto firstPlayable = std::find_if(
        items.begin(), items.end(), [](const MovieItem& item) { return !item.id.isEmpty() && isPlayableItem(item); });
    if (firstPlayable == items.end()) {
        showToast(QStringLiteral("This list has no playable items."));
        return;
    }
    playQueuedItems(items, static_cast<int>(std::distance(items.begin(), firstPlayable)), false);
    if (shuffled)
        m_playQueue->setShuffled(true);
}

void AppController::queueEpisodicContainer(const QString& seriesId, const QString& seasonId, bool next)
{
    if (seriesId.isEmpty())
        return;
    Async::runScoped(
        this, m_catalog->fetchEpisodes(seriesId, seasonId),
        [this, next](const std::vector<MovieItem>& episodes) {
            if (episodes.empty()) {
                setErrorText(QStringLiteral("There is nothing here to queue."));
                return;
            }
            if (!m_playQueue->addToQueue(episodes, next))
                setErrorText(QStringLiteral("This item cannot be queued."));
        },
        [this](const std::exception_ptr& error) {
            qWarning() << "queue: episode lookup failed" << exceptionMessage(error);
            setErrorText(QStringLiteral("Could not reach the server to queue that."));
        });
}

void AppController::playEpisodicContainer(const QString& seriesId, const QString& seasonId)
{
    if (seriesId.isEmpty())
        return;
    if (m_episodeQueuePending)
        return;
    m_episodeQueuePending = true;

    const quint64 generation = ++m_episodeQueueGeneration;
    setBusy(true,
        seasonId.isEmpty() ? QStringLiteral("Finding the next episode…")
                           : QStringLiteral("Finding the next episode in this season…"));
    Async::runScoped(
        this, m_catalog->fetchEpisodes(seriesId, seasonId),
        [this, generation](const std::vector<MovieItem>& episodes) {
            if (generation != m_episodeQueueGeneration)
                return;
            m_episodeQueuePending = false;
            const int startIndex = episodicPlaybackStartIndex(episodes);
            if (startIndex < 0) {
                setBusy(false);
                const bool watchedEpisode = std::any_of(
                    episodes.cbegin(), episodes.cend(), [](const MovieItem& episode) { return episode.played; });
                showToast(watchedEpisode
                        ? QStringLiteral("There is no unplayed episode after the last watched episode.")
                        : QStringLiteral("No playable episodes are available."));
                return;
            }
            playQueuedItems(episodes, startIndex, false);
        },
        [this, generation](const std::exception_ptr& error) {
            if (generation != m_episodeQueueGeneration)
                return;
            m_episodeQueuePending = false;
            setBusy(false);
            showToast(exceptionMessage(error));
        },
        "episodic container playback");
}

void AppController::cancelEpisodicPlaybackSelection()
{
    if (!m_episodeQueuePending)
        return;
    ++m_episodeQueueGeneration;
    m_episodeQueuePending = false;
    setBusy(false);
}

void AppController::playQueuedItem(const MovieItem& item, bool fromStart)
{
    if (item.itemType == QStringLiteral("Episode") && !item.seriesId.isEmpty()) {
        playEpisodeWithContext(item, 0, fromStart);
        return;
    }
    if (!m_playQueue->playNow(item)) {
        showToast(QStringLiteral("This item cannot be queued."));
        return;
    }
    startQueuedPlayback(fromStart);
}

void AppController::playEpisodeWithContext(const MovieItem& episode, int direction, bool fromStart)
{
    if (episode.itemType != QStringLiteral("Episode") || episode.seriesId.isEmpty()) {
        if (direction == 0 && m_playQueue->playNow(episode))
            startQueuedPlayback(fromStart);
        return;
    }

    const quint64 generation = ++m_episodeQueueGeneration;
    m_episodeQueuePending = true;
    setBusy(true, direction == 0 ? QStringLiteral("Loading episode queue…") : QStringLiteral("Finding episode…"));
    Async::runScoped(
        this, m_catalog->fetchEpisodes(episode.seriesId),
        [this, generation, episode, direction, fromStart](const std::vector<MovieItem>& episodes) {
            if (generation != m_episodeQueueGeneration)
                return;
            m_episodeQueuePending = false;
            const auto current = std::find_if(episodes.begin(), episodes.end(),
                [&episode](const MovieItem& candidate) { return candidate.id == episode.id; });
            if (current == episodes.end()) {
                setBusy(false);
                if (direction == 0 && m_playQueue->playNow(episode))
                    startQueuedPlayback(fromStart);
                else
                    showToast(QStringLiteral("This episode was not found in its series."));
                return;
            }

            int targetIndex = static_cast<int>(std::distance(episodes.begin(), current));
            if (direction != 0) {
                int candidate = targetIndex + direction;
                while (candidate >= 0 && candidate < static_cast<int>(episodes.size())
                    && !isPlayableItem(episodes[static_cast<size_t>(candidate)])) {
                    candidate += direction;
                }
                if (candidate < 0 || candidate >= static_cast<int>(episodes.size())) {
                    setBusy(false);
                    showToast(direction < 0 ? QStringLiteral("There is no previous episode.")
                                            : QStringLiteral("There is no next episode."));
                    return;
                }
                targetIndex = candidate;
            }

            if (!m_playQueue->playNow(episodes, targetIndex)) {
                setBusy(false);
                showToast(QStringLiteral("The adjacent episode could not be queued."));
                return;
            }
            qInfo() << "play queue: loaded episode context" << episodes.size() << "items, target" << targetIndex;
            startQueuedPlayback(direction == 0 ? fromStart : true);
        },
        [this, generation, episode, direction, fromStart](const std::exception_ptr& error) {
            if (generation != m_episodeQueueGeneration)
                return;
            m_episodeQueuePending = false;
            setBusy(false);
            if (direction == 0 && m_playQueue->playNow(episode)) {
                startQueuedPlayback(fromStart);
                return;
            }
            showToast(exceptionMessage(error));
        },
        "episode queue context");
}

void AppController::startQueuedPlayback(bool fromStart)
{
    if (!inGroup()) {
        playQueueCurrent(fromStart);
        return;
    }

    const std::vector<PlaybackQueueItem> queue = m_playQueue->nowPlayingQueue();
    QStringList itemIds;
    itemIds.reserve(static_cast<qsizetype>(queue.size()));
    for (const PlaybackQueueItem& item : queue)
        itemIds.push_back(item.itemId);

    const MovieItem item = m_playQueue->currentItem();
    const qint64 startPositionTicks
        = fromStart || !isMeaningfulResumePosition(item.resumeTicks, item.runtimeTicks) ? 0 : item.resumeTicks;
    setBusy(true, QStringLiteral("Updating SyncPlay queue…"));
    const RequestGeneration::Token generation = m_syncPlayQueueRequestGeneration.next();
    // Arm this before SetNewQueue: the websocket PlayQueue update can arrive
    // before or after the HTTP response. Waiting for the response allowed an
    // old loaded session to consume the pending unpause request.
    m_group->requestUnpauseWhenReady();
    Async::runScoped(
        this, m_group->publishQueue(itemIds, m_playQueue->currentIndex(), startPositionTicks), []() {},
        [this, generation](const std::exception_ptr& error) {
            if (!m_syncPlayQueueRequestGeneration.isCurrent(generation))
                return;
            m_group->cancelPendingUnpause();
            setBusy(false);
            showToast(exceptionMessage(error));
        },
        "syncplay queue update");
}

void AppController::playQueueCurrent(bool fromStart)
{
    MovieItem item = m_playQueue->currentItem();
    if (item.id.isEmpty())
        return;
    if (fromStart || !isMeaningfulResumePosition(item.resumeTicks, item.runtimeTicks))
        item.resumeTicks = 0;
    m_activePlaybackItem = item;
    if (item.itemType == QStringLiteral("Movie") && item.people.isEmpty()) {
        const QString itemId = item.id;
        Async::runScoped(
            this, m_catalog->fetchItemDetails(itemId),
            [this, itemId](const MovieItem& details) { m_playQueue->updatePeople(itemId, details.people); },
            [itemId](const std::exception_ptr& error) {
                qInfo() << "app: movie credits unavailable for" << itemId << ":" << exceptionMessage(error);
            },
            "movie credits");
    }
    setBusy(true, QStringLiteral("Negotiating playback…"));
    Async::runScoped(
        this, startPlayback(item), []() {},
        [this](const std::exception_ptr& error) {
            setBusy(false);
            showToast(exceptionMessage(error));
        },
        "playback startup");
}

QCoro::Task<void> AppController::startPlayback(MovieItem playItem, bool startPaused, bool forceTranscode,
    int audioStreamIndex, int subtitleStreamIndex, bool restartActive)
{
    Diagnostics::Task task(QStringLiteral("playback_negotiate"),
        { { QStringLiteral("itemId"), playItem.id }, { QStringLiteral("title"), playItem.title },
            { QStringLiteral("type"), playItem.itemType } });
    const RequestGeneration::Token generation = m_playbackLoadGeneration.next();

    if (!forceTranscode)
        m_codecFallbackAttempted = false;
    PlaybackSession session = co_await m_playback->resolvePlayback(playItem, forceTranscode);
    if (!m_playbackLoadGeneration.isCurrent(generation))
        co_return;
    if (!forceTranscode && playItem.itemType == QStringLiteral("Episode") && !playItem.seriesId.isEmpty()) {
        const QByteArray storedSelection
            = co_await m_database->loadCacheEntryAsync(QString::fromLatin1(kSeriesTrackSelectionNamespace),
                seriesTrackSelectionKey(m_catalog->libraryScopeKey(), playItem.seriesId));
        if (!m_playbackLoadGeneration.isCurrent(generation))
            co_return;
        SeriesAudioPreference storedAudioPreference;
        int storedSubtitleStreamIndex = -1;
        if (decodeTrackSelection(storedSelection, storedAudioPreference, storedSubtitleStreamIndex)) {
            const int matchingAudioStreamIndex
                = m_settings->value(QString::fromLatin1(kRememberSeriesAudioTrackKey)).toBool()
                ? matchingSeriesAudioStreamIndex(session.mediaStreams, storedAudioPreference)
                : -1;
            if (matchingAudioStreamIndex >= 0) {
                session.audioStreamIndex = matchingAudioStreamIndex;
                qInfo() << "app: restoring series audio preference" << storedAudioPreference.language << "track"
                        << storedAudioPreference.languageTrackNumber << "stream" << matchingAudioStreamIndex;
            }
            session.subtitleStreamIndex = storedSubtitleStreamIndex;
            session.restoreStreamSelection = true;
        }
    }
    if (restartActive) {
        // Stop/navigation during negotiation must not resurrect this stream.
        if (!m_player->sessionActive() || m_activePlaybackItem.id != playItem.id) {
            setBusy(false);
            co_return;
        }
        startPaused = m_player->paused();
        audioStreamIndex = m_activeAudioStreamIndex;
        subtitleStreamIndex = m_activeSubtitleStreamIndex;
    }
    const std::vector<PlaybackQueueItem> queue = m_playQueue->nowPlayingQueue();
    if (forceTranscode) {
        PlaybackFailurePolicy::prepareFallbackSession(session, queue, audioStreamIndex, subtitleStreamIndex);
    } else {
        session.nowPlayingQueue = queue;
        // A restart that is not a codec fallback - a quality change - still has
        // to land on the tracks the viewer had chosen.
        if (audioStreamIndex != -2 || subtitleStreamIndex != -2) {
            if (audioStreamIndex >= 0)
                session.audioStreamIndex = audioStreamIndex;
            if (subtitleStreamIndex != -2)
                session.subtitleStreamIndex = subtitleStreamIndex;
            session.restoreStreamSelection = true;
        }
    }
    m_activePlaybackStreams = session.mediaStreams;
    const int fileAudioDelayMs = restartActive ? m_player->fileAudioDelayMs() : 0;
    const int subtitleDelayMs = restartActive ? m_player->subtitleDelayMs() : 0;
    setBusy(false);
    m_player->play(session, startPaused);
    if (restartActive) {
        m_player->setFileAudioDelayMs(fileAudioDelayMs);
        m_player->setSubtitleDelayMs(subtitleDelayMs);
    }

    const QString itemId = playItem.id;
    Async::runScoped(
        this, m_playback->fetchMediaSegments(itemId),
        [this, itemId](const std::vector<MediaSegment>& segments) {
            if (m_player && !segments.empty())
                m_player->setMediaSegments(itemId, segments);
        },
        [itemId](const std::exception_ptr& error) {
            qInfo() << "app: media segments unavailable for" << itemId << ":" << exceptionMessage(error);
        });
}

void AppController::onMemoryPressure(const QString& level)
{
    const QString normalized = level.trimmed().toLower();
    if (normalized != QStringLiteral("low") && normalized != QStringLiteral("critical")) {
        return;
    }

    const bool aggressive = normalized == QStringLiteral("critical");
    qInfo() << "memory pressure:" << normalized << "aggressive=" << aggressive;
    if (m_artwork)
        m_artwork->releaseMemory(aggressive);
    QPixmapCache::clear();
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
    if (aggressive)
        emit aggressiveMemoryPressure();
}

void AppController::shutdown()
{
    Diagnostics::Phase phase(QStringLiteral("shutdown"), QStringLiteral("app_controller_shutdown"));
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    qInfo() << "app: shutdown requested";
    m_player->prepareForShutdown();
    m_prefetch->stop();
    m_provider->shutdown();
    m_player->teardownMpv();
}

void AppController::clearError()
{
    setErrorText({});
}

void AppController::clearLogs()
{
    emit clearLogsRequested();
    emit toastMessage(QStringLiteral("Logs cleared."));
}
QString AppController::diagnosticsPreview() const
{
#ifdef SPOOL_ANDROID
    return QStringLiteral(
        "Creates a ZIP containing the app log, mpv log, rotated logs, and a system report, then opens Android’s "
        "share menu. Logs are intended for private support channels; review the recipient before sending.\n\n%1")
        .arg(Diagnostics::supportReportPreview());
#else
    return Diagnostics::supportReportPreview();
#endif
}

QString AppController::saveDiagnosticsReport()
{
    const QString path = Diagnostics::saveSupportReport();
    if (path.isEmpty()) {
        emit toastMessage(QStringLiteral("Could not save diagnostics."));
        return {};
    }
#ifdef SPOOL_ANDROID
    emit diagnosticsReportSaved(path);
#else
    emit toastMessage(QStringLiteral("Diagnostics saved to %1").arg(path));
#endif
    return path;
}

void AppController::setBusy(bool busy, const QString& busyText)
{
    if (m_busy == busy && m_busyText == busyText)
        return;
    m_busy = busy;
    m_busyText = busyText;
    emit busyChanged();
}

void AppController::setErrorText(const QString& errorText)
{
    if (m_errorText == errorText)
        return;
    m_errorText = errorText;
    emit errorTextChanged();
}

void AppController::showToast(const QString& message)
{
    if (message.trimmed().isEmpty())
        return;
    emit toastMessage(message);
}

void AppController::loadLibraries()
{
    m_prefetch->stop();
    Async::runScoped(
        this, m_catalog->fetchLibraries(),
        [this](const std::vector<LibraryItem>& libraries) {
            m_libraries.setLibraries(libraries);
            setBusy(false);
            refreshHomeRows();
        },
        [this](const std::exception_ptr& error) {
            setBusy(false);
            setErrorText(exceptionMessage(error));
        });
}

void AppController::loadLibraryFilterOptions(RequestGeneration::Token generation, const LibraryItem& library)
{
    if (!m_catalog->signedIn() || library.id.isEmpty())
        return;

    Async::runLatest(
        this, m_catalog->fetchLibraryFilterOptions(library.id, library.collectionType), m_libraryLoadGeneration,
        generation,
        [this, library](const QVariantMap& options) {
            if (library.id != m_browse->libraryId())
                return;
            m_browse->setFilterOptions(options);
        },
        [this, library](const std::exception_ptr& error) {
            if (library.id != m_browse->libraryId())
                return;
            qWarning() << "library filters: failed" << library.name << exceptionMessage(error);
            m_browse->clearFilterOptions();
        });
}

void AppController::showCurrentItemsPage(const PagedMovieItems& page, const QString& cacheKey, bool append)
{
    m_browse->setPage(page, cacheKey, append);
    // Keep the warm cache in sync with what the user just saw so the next
    // open of this library can skip the refresh while the data is fresh.
    if (!append && page.startIndex == 0 && m_prefetch)
        m_prefetch->storePage(cacheKey, page);
    setBusy(false);
}

RequestGeneration::Token AppController::beginBrowse(bool useWarmCache)
{
    const BrowseDescriptor descriptor = m_browse->descriptor();
    if (!descriptor.isValid() || !m_catalog->signedIn())
        return 0;

    const RequestGeneration::Token generation = m_libraryLoadGeneration.next();
    const QVariantMap query = descriptor.kind == BrowseKind::Library ? m_browse->query() : QVariantMap {};
    QString cacheKey = descriptor.cacheKey(query);
    if (descriptor.kind == BrowseKind::Library) {
        LibraryItem library;
        library.id = m_browse->libraryId();
        library.collectionType = m_browse->libraryCollectionType();
        cacheKey = libraryCacheKey(library, query);
    }

    m_browse->resetPaging(cacheKey);
    m_prefetch->stop();
    if (m_artwork)
        m_artwork->cancelPrefetches();
    if (useWarmCache) {
        const int cachedCount = m_browse->applyCachedPage(cacheKey);
        m_browse->setWarmCachePaging(cachedCount, kLibraryPageSize);
        if (cachedCount > 0) {
            const qint64 ageMs = m_prefetch ? m_prefetch->pageAgeMs(cacheKey) : -1;
            if (ageMs >= 0 && ageMs < kFreshLibraryCacheMs) {
                qInfo() << "library open: cache fresh, skipping refresh" << descriptor.name << cachedCount
                        << "age_ms=" << ageMs;
                m_browse->setLoadingMore(false);
                return generation;
            }
            qInfo() << "library open: showing cached page while refreshing" << descriptor.name << cachedCount;
        }
    } else {
        m_browse->clear();
        m_browse->setLoadingMore(true);
    }

    Async::runLatest(
        this, m_catalog->fetchBrowsePage(descriptor, 0, kLibraryPageSize, query), m_libraryLoadGeneration, generation,
        [this, cacheKey](const PagedMovieItems& page) { showCurrentItemsPage(page, cacheKey, false); },
        [this](const std::exception_ptr& error) {
            m_browse->setLoadingMore(false);
            showToast(exceptionMessage(error));
        });
    return generation;
}

void AppController::openNamedCollection(const QString& kind, const QString& value, const QString& collectionType)
{
    const QString name = value.trimmed();
    if (name.isEmpty() || (kind != QStringLiteral("genre") && kind != QStringLiteral("studio")))
        return;
    m_browse->enterNamedCollection(kind, name, collectionType);
    beginBrowse();
}

QVariantList AppController::streamingQualityOptions() const
{
    const qint64 override = m_quality ? m_quality->bitrateOverride() : m_genericBitrateOverride;
    const int heightOverride = m_quality ? m_quality->heightOverride() : m_genericHeightOverride;
    QVariantList options;
    options.push_back(QVariantMap {
        { QStringLiteral("label"), QStringLiteral("Auto") },
        { QStringLiteral("detail"), m_quality ? m_quality->autoDescription() : QStringLiteral("Direct Play") },
        { QStringLiteral("bitrate"), 0 },
        { QStringLiteral("height"), 0 },
        { QStringLiteral("selected"), override <= 0 && heightOverride <= 0 },
    });

    qint64 sourceBitrate = 0;
    int sourceHeight = 0;
    for (const MediaSourceInfo& source : m_activePlaybackItem.mediaSources) {
        sourceBitrate = std::max<qint64>(sourceBitrate, source.bitRate);
        for (const MediaStreamInfo& stream : source.streams) {
            if (stream.type == QStringLiteral("Video") && stream.height > 0)
                sourceHeight = std::max(sourceHeight, stream.height);
        }
    }

    std::vector<StreamQualityControl::Rung> rungs;
    if (m_quality)
        rungs = m_quality->ladder(sourceBitrate, sourceHeight);
    if (rungs.empty())
        rungs = StreamQualityControl::defaultLadder(sourceBitrate, sourceHeight);

    for (const StreamQualityControl::Rung& rung : rungs) {
        options.push_back(QVariantMap {
            { QStringLiteral("label"), rung.label },
            { QStringLiteral("detail"), QString() },
            { QStringLiteral("bitrate"), rung.bitrate },
            { QStringLiteral("height"), rung.height },
            { QStringLiteral("selected"),
                override == rung.bitrate && (heightOverride == 0 || heightOverride == rung.height) },
        });
    }
    return options;
}

void AppController::selectStreamingQuality(qint64 bitrate, int height)
{
    const qint64 previousBitrate = m_quality ? m_quality->bitrateOverride() : m_genericBitrateOverride;
    const int previousHeight = m_quality ? m_quality->heightOverride() : m_genericHeightOverride;
    if (previousBitrate == bitrate && previousHeight == height)
        return;
    if (m_quality)
        m_quality->setOverride(bitrate, height);
    else {
        m_genericBitrateOverride = bitrate;
        m_genericHeightOverride = height;
    }
    emit streamingQualityChanged();

    // The source chose direct play or transcoding against the ceiling it was
    // given when the stream was resolved, so a new ceiling only takes effect
    // through a fresh resolution. Resume where the viewer was, keeping the
    // audio and subtitle tracks they had selected.
    if (!m_player->sessionActive() || m_activePlaybackItem.id.isEmpty())
        return;
    const qint64 positionTicks = static_cast<qint64>(m_player->positionSeconds() * 10'000'000.0);
    const MovieItem resumeItem = PlaybackFailurePolicy::retryItem(m_activePlaybackItem, positionTicks);
    const int audioStreamIndex = m_activeAudioStreamIndex;
    const int subtitleStreamIndex = m_activeSubtitleStreamIndex;
    m_qualityFallbackBitrate = previousBitrate;
    m_qualityFallbackHeight = previousHeight;
    // Resolving again is a network round trip. Leaving the old core up for it
    // keeps the picture on screen until the replacement is ready to start;
    // play() still tears it down synchronously before the new one begins.
    setBusy(true, QStringLiteral("Changing quality…"));
    Async::runScoped(
        this, startPlayback(resumeItem, false, false, audioStreamIndex, subtitleStreamIndex), []() {},
        [this](const std::exception_ptr& error) {
            setBusy(false);
            showToast(exceptionMessage(error));
        },
        "playback quality change");
}

void AppController::handlePlaybackStopped(const QString& itemId, qint64 positionTicks, bool completed)
{
    qInfo() << "app: playback stopped" << itemId << positionTicks << completed;
    m_itemState->recordPlaybackStopped(m_activePlaybackItem, itemId, positionTicks, completed);
    m_playQueue->updateResumeTicks(itemId, completed ? 0 : positionTicks);
    if (!completed || m_activePlaybackItem.id != itemId || inGroup()) {
        setPlaybackTransition(false);
        return;
    }
    bool continueQueue = false;
    if (m_repeatMode == QStringLiteral("RepeatOne")) {
        continueQueue = m_playQueue->currentIndex() >= 0;
    } else {
        continueQueue = m_playQueue->next();
        if (!continueQueue && m_repeatMode == QStringLiteral("RepeatAll") && m_playQueue->rowCount() > 0)
            continueQueue = m_playQueue->playAt(0);
    }
    if (continueQueue) {
        // Hold the surface across the gap while the next (or repeated) item is
        // negotiated and decoded.
        setPlaybackTransition(true);
        playQueueCurrent(m_repeatMode == QStringLiteral("RepeatOne"));
    } else {
        setPlaybackTransition(false);
        m_playQueue->enqueueEpisodeSuccessors(m_activePlaybackItem);
    }
}

void AppController::setPlaybackTransition(bool transition)
{
    if (m_playbackTransition == transition)
        return;
    m_playbackTransition = transition;
    emit playbackTransitionChanged();
    if (!transition)
        return;

    // Normally the next item becoming visible clears this. If it never
    // arrives — a negotiate that fails or hangs — the shell must not be left
    // holding a player surface over nothing.
    const quint64 generation = ++m_playbackTransitionGeneration;
    QTimer::singleShot(kPlaybackTransitionTimeoutMs, this, [this, generation]() {
        if (generation == m_playbackTransitionGeneration)
            setPlaybackTransition(false);
    });
}

} // namespace Spool
