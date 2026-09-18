#pragma once

#include "../cache/DatabaseManager.h"
#include "../common/RequestGeneration.h"
#include "../discovery/DiscoveryController.h"
#include "../media/MediaTypes.h"
#include "../models/DiscoveredServerModel.h"
#include "../models/LibraryListModel.h"
#include "../models/MovieGridModel.h"
#include "../player/PlayQueueController.h"
#include "../player/PlayerController.h"
#include "BrowseSessionController.h"
#include "ContentModelController.h"
#include "HomeModelController.h"
#include "LibraryManagementController.h"
#include "QuickConnectController.h"
#include "RemoteControlController.h"
#include "SearchController.h"
#include "SessionController.h"
#include "SettingsController.h"
#include "SpoolRemoteProtocol.h"
#include "SyncPlayController.h"

#include <QCoroTask>
#include <QLockFile>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

#include <vector>

namespace JellyfinNative {

class ArtworkService;
class JellyfinApiFacade;
class LibraryPrefetchController;
class UserItemStateController;
class TlsTrustController;
class AppController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy MEMBER m_busy NOTIFY busyChanged)
    // True only while the queue is stepping from one item to the next, so the
    // shell can hold the player surface up instead of dropping to the page
    // behind it for the length of a negotiate.
    Q_PROPERTY(bool playbackTransition MEMBER m_playbackTransition NOTIFY playbackTransitionChanged)
    Q_PROPERTY(QString busyText MEMBER m_busyText NOTIFY busyChanged)
    Q_PROPERTY(QString errorText MEMBER m_errorText NOTIFY errorTextChanged)
    Q_PROPERTY(bool hasDefaultProfile MEMBER m_hasDefaultProfile NOTIFY defaultProfileChanged)
    Q_PROPERTY(bool initialized READ initialized NOTIFY initializedChanged)

public:
    AppController(DatabaseManager *database, DiscoveryController *discovery, JellyfinApiFacade *api,
        ArtworkService *artwork, PlayerController *player, TlsTrustController *tlsTrust, QObject *parent = nullptr);

    DiscoveredServerModel *discoveredServers()
    {
        return &m_discoveredServers;
    }
    ArtworkService *artwork() const
    {
        return m_artwork;
    }
    LibraryListModel *libraries()
    {
        return &m_libraries;
    }
    BrowseSessionController *browse()
    {
        return m_browse;
    }
    HomeModelController *home()
    {
        return m_home;
    }
    ContentModelController *content()
    {
        return m_content;
    }
    UserItemStateController *itemState()
    {
        return m_itemState;
    }
    SearchController *search()
    {
        return m_search;
    }
    PlayerController *player()
    {
        return m_player;
    }
    PlayQueueController *playQueue()
    {
        return m_playQueue;
    }
    SyncPlayController *syncPlay()
    {
        return m_syncPlay;
    }
    RemoteControlController *remoteControl()
    {
        return m_remoteControl;
    }
    SettingsController *settings()
    {
        return m_settings;
    }
    bool initialized() const
    {
        return m_initialized;
    }
    SessionController *session()
    {
        return m_session;
    }
    QuickConnectController *quickConnect()
    {
        return m_quickConnect;
    }
    LibraryManagementController *management()
    {
        return m_management;
    }

    Q_INVOKABLE void initialize();
    void shutdown();
    Q_INVOKABLE void chooseDiscoveredServer(int index);
    Q_INVOKABLE void rememberServer(const QString& name, const QString& address);
    Q_INVOKABLE void useProfile(const QString& profileId);
    Q_INVOKABLE void switchUser();
    Q_INVOKABLE void logout();
    Q_INVOKABLE void goHome();
    Q_INVOKABLE void openLibrary(int index);
    Q_INVOKABLE bool openLibraryById(const QString& libraryId);
    Q_INVOKABLE void playFromModel(QObject *model, int index, bool fromStart = false);
    Q_INVOKABLE void playItemId(const QString& itemId, bool fromStart = false);
    void stopPlayback();
    Q_INVOKABLE void playQueueNext();
    Q_INVOKABLE void playQueuePrevious();
    Q_INVOKABLE void playQueueItem(int index);
    // The queue panel edits through these rather than reaching for the
    // PlayQueue singleton, so a SyncPlay group cannot be desynchronised by a
    // drag that never passed a guard. A reorder gesture previews locally on
    // every step — waiting on a round trip per step would make a held D-pad key
    // unusable — and publishes once, on drop.
    Q_INVOKABLE bool queueEditable() const;
    Q_INVOKABLE bool previewQueueMove(int from, int to);
    Q_INVOKABLE void commitQueueMove(int from, int to);
    // The same pair for a folded run dragged as one block.
    Q_INVOKABLE bool previewQueueMoveRange(int from, int count, int to);
    Q_INVOKABLE void commitQueueMoveRange(int from, int count, int to);
    Q_INVOKABLE void removeQueueItem(int index);
    Q_INVOKABLE void playNextFromItem(const MovieItem& item);
    Q_INVOKABLE void addToQueueFromItem(const MovieItem& item);
    // Queue every episode of a season or series without opening it first.
    Q_INVOKABLE void queueEpisodicContainer(const QString& seriesId, const QString& seasonId, bool next);
    Q_INVOKABLE void playModel(MovieGridModel *model, bool shuffled = false);
    Q_INVOKABLE void playEpisodicContainer(const QString& seriesId, const QString& seasonId = {});
    Q_INVOKABLE void cancelEpisodicPlaybackSelection();
    Q_INVOKABLE void openNamedCollection(const QString& kind, const QString& name, const QString& collectionType = {});
    Q_INVOKABLE void onMemoryPressure(const QString& level);
    Q_INVOKABLE void clearError();
    Q_INVOKABLE void clearLogs();
    Q_INVOKABLE QString diagnosticsPreview() const;
    Q_INVOKABLE QString saveDiagnosticsReport();

    // Quality picker for the player overlay. Row zero is Auto and carries a
    // description of where the automatic ceiling came from; the rest are fixed
    // ceilings below the source bitrate. Choosing one restarts the stream at
    // the current position because the server decides direct play against the
    // ceiling it was handed at negotiation time.
    Q_INVOKABLE QVariantList streamingQualityOptions() const;
    Q_INVOKABLE void selectStreamingQuality(qint64 bitrate, int height = 0);

signals:
    void busyChanged();
    void playbackTransitionChanged();
    void streamingQualityChanged();
    void errorTextChanged();
    void defaultProfileChanged();
    void initializedChanged();
    void aggressiveMemoryPressure();
    void toastMessage(const QString& message);
    void remoteUiActionRequested(const QString& action);
    void remoteMessageRequested(const QString& message);
    void remoteContentRequested(const QString& itemId, const QString& itemType, const QString& title);
    void remoteSeekPreviewRequested(qint64 positionTicks, bool active);
    void clearLogsRequested();
    void diagnosticsReportSaved(const QString& path);

private:
    int claimInstanceSlot();
    void setBusy(bool busy, const QString& busyText = {});
    void setErrorText(const QString& errorText);
    void showToast(const QString& message);
    QCoro::Task<void> initializeAsync();
    void resetApplicationState();
    QCoro::Task<void> applyDiscoveredServersCacheAsync();
    void cacheDiscoveredServers();
    void loadLibraries();
    void loadMoreCurrentItems();
    void refreshHomeRows()
    {
        m_home->refresh(m_libraries.libraries());
    }
    void showCurrentItemsPage(const PagedMovieItems& page, const QString& cacheKey, bool append);
    void loadLibraryFilterOptions(RequestGeneration::Token generation, const LibraryItem& library);
    RequestGeneration::Token beginBrowse(bool useWarmCache = false);
    QCoro::Task<void> startPlayback(MovieItem playItem, bool startPaused = false, bool forceTranscode = false,
        int audioStreamIndex = -2, int subtitleStreamIndex = -2, bool restartActive = false);
    void playQueuedItems(const std::vector<MovieItem>& items, int startIndex, bool fromStart = false);
    bool modelIsOrderedList(MovieGridModel *model) const;
    void playAlbumFrom(const MovieItem& track, bool fromStart);
    bool inSyncPlayGroup() const;
    void setPlaybackTransition(bool transition);
    QString queuePlaylistItemId(int index) const;
    bool enqueueForGroup(const MovieItem& item, bool queueNext);
    void playQueuedItem(const MovieItem& item, bool fromStart = false);
    void playEpisodeWithContext(const MovieItem& episode, int direction, bool fromStart);
    void playQueueCurrent(bool fromStart = false);
    void startQueuedPlayback(bool fromStart = false);
    void handleRemotePlay(const QJsonObject& data);
    void handleSpoolMessage(const SpoolRemoteProtocol::Message& message);
    void handleRemotePlaystate(const QJsonObject& data);
    void handleRemoteGeneralCommand(const QJsonObject& data);
    // Folder-like containers open their child listing; everything else plays directly.
    void playOrOpen(const MovieItem& item, bool fromStart = false);
    void handlePlaybackStopped(const QString& itemId, qint64 positionTicks, bool completed);

    DatabaseManager *m_database = nullptr;
    DiscoveryController *m_discovery = nullptr;
    JellyfinApiFacade *m_api = nullptr;
    ArtworkService *m_artwork = nullptr;
    PlayerController *m_player = nullptr;
    SyncPlayController *m_syncPlay = nullptr;
    RemoteControlController *m_remoteControl = nullptr;
    PlayQueueController *m_playQueue = nullptr;
    ContentModelController *m_content = nullptr;
    BrowseSessionController *m_browse = nullptr;
    HomeModelController *m_home = nullptr;
    QuickConnectController *m_quickConnect = nullptr;
    SettingsController *m_settings = nullptr;
    SessionController *m_session = nullptr;
    LibraryPrefetchController *m_prefetch = nullptr;
    UserItemStateController *m_itemState = nullptr;
    SearchController *m_search = nullptr;
    quint64 m_episodeQueueGeneration = 0;
    quint64 m_albumQueueGeneration = 0;
    bool m_episodeQueuePending = false;
    LibraryManagementController *m_management = nullptr;
    DiscoveredServerModel m_discoveredServers;
    LibraryListModel m_libraries;
    MovieItem m_activePlaybackItem;
    QList<MediaStreamInfo> m_activePlaybackStreams;
    int m_activeAudioStreamIndex = -1;
    int m_activeSubtitleStreamIndex = -1;
    bool m_busy = false;
    bool m_playbackTransition = false;
    quint64 m_playbackTransitionGeneration = 0;
    bool m_hasDefaultProfile = false;
    bool m_initialized = false;
    QString m_busyText;
    QString m_errorText;
    QString m_remoteRepeatMode = QStringLiteral("RepeatNone");
    RequestGeneration m_libraryLoadGeneration;
    RequestGeneration m_playbackLoadGeneration;
    RequestGeneration m_syncPlayQueueRequestGeneration;
    RequestGeneration m_remotePlaybackRequestGeneration;
    QString m_remotePlaybackFingerprint;
    bool m_shuttingDown = false;
    bool m_codecFallbackAttempted = false;
    // The ceiling a quality change moved away from, kept only until that
    // stream loads, so a stream the server cannot deliver falls back to the
    // one that was playing instead of ending playback.
    qint64 m_qualityFallbackBitrate = -1;
    int m_qualityFallbackHeight = 0;
    // Held for the process lifetime so a second local instance picks the next
    // slot and reports a device identity of its own.
    std::unique_ptr<QLockFile> m_instanceLock;
};

} // namespace JellyfinNative
