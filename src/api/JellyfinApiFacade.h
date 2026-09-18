#pragma once

#include "../media/MediaTypes.h"
#include "HttpRequestPolicy.h"
#include "JellyfinSession.h"
#include "PlaybackBandwidthPolicy.h"

#include <QCoroTask>

#include <QDateTime>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequestFactory>
#include <QObject>
#include <QRestAccessManager>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrlQuery>
#include <QVariantMap>

#include <vector>

namespace JellyfinNative {

class TlsTrustController;
class JellyfinApiFacade final : public QObject {
    Q_OBJECT

public:
    explicit JellyfinApiFacade(
        QNetworkAccessManager *networkAccessManager, TlsTrustController *tlsTrust, QObject *parent = nullptr);
    ~JellyfinApiFacade() override;

    void setServerUrl(const QString& serverUrl);
    QString serverUrl() const;

    void setDeviceIdentity(const QString& deviceId, const QString& deviceName, const QString& clientVersion);
    void setDeviceId(const QString& deviceId);
    // Some platforms only learn their user-visible name asynchronously. The
    // name rides the authorization header of every request, so the server
    // picks a later one up on its own.
    void setDeviceName(const QString& deviceName);
    QString deviceId() const;

    // Forwarded into the QNetworkRequestFactory common headers so every API
    // call hints the server about our locale. Jellyfin uses this to localise
    // server-returned strings (Continue Watching titles, etc.).
    void setAcceptLanguage(const QString& bcp47Tag);

    void setSession(const AuthSession& session);
    AuthSession session() const;
    void setPlaybackPreferences(
        qint64 manualMaxStreamingBitrate, bool unlimitedLocalNetwork, bool preferRemux, int maxStreamingHeight = 0);
    void setVideoCodecCapabilities(QStringList videoCodecs, bool restrictVideoCodecs);
    void setRemoteControlTargetEnabled(bool enabled);
    bool remoteControlTargetEnabled() const
    {
        return m_remoteControlTargetEnabled;
    }
    int playbackParallelRequests() const;
    QCoro::Task<void> refreshPlaybackNetworkState();

    // Measuring pulls several megabytes from the server, so it must never run
    // beside a stream. Playback that starts before a measurement lands keeps
    // whatever ceiling is known, which is a remembered one where the route has
    // been seen before and a conservative estimate otherwise.
    void setPlaybackActive(bool active);
    qint64 maxStreamingBitrate() const;
    qint64 measuredStreamingBitrate() const;
    PlaybackBandwidthPolicy::Source streamingBitrateSource() const;

    // A ceiling chosen from the player overlay. It outranks every automatic
    // input and the Settings limit, and it lasts only as long as this session:
    // a quality picked because the train Wi-Fi is bad should not still be
    // capping playback at home tomorrow. Zero restores automatic selection.
    void setSessionBitrateOverride(qint64 bitrate);
    qint64 sessionBitrateOverride() const;

    // Resolution is settled separately from bitrate: a viewer sparing a slow
    // decoder wants a smaller picture at the bitrate they already have, and a
    // viewer on a metered connection wants fewer bits at the size they already
    // have. The session override outranks the Settings ceiling and lasts only
    // as long as this session; zero means no ceiling from that source.
    void setSessionHeightOverride(int height);
    int sessionHeightOverride() const;
    int maxStreamingHeight() const;
    QString authorizationHeader(const QString& tokenOverride = {}) const;
    void cancelRequests();

    QCoro::Task<AuthSession> authenticateByName(QString username, QString password);
    QCoro::Task<bool> quickConnectEnabled();
    QCoro::Task<QJsonObject> initiateQuickConnect();
    QCoro::Task<QJsonObject> pollQuickConnect(QString secret);
    QCoro::Task<AuthSession> authenticateWithQuickConnect(QString secret);
    QCoro::Task<QString> fetchCurrentUserName();
    QCoro::Task<QJsonObject> fetchUserConfiguration();
    QCoro::Task<void> updateUserConfiguration(QJsonObject configuration);
    QCoro::Task<QJsonObject> fetchCurrentUserPolicy();
    QCoro::Task<QJsonArray> fetchCultures();
    QCoro::Task<std::vector<LibraryItem>> fetchLibraries();
    QCoro::Task<PagedMovieItems> fetchBrowsePage(
        BrowseDescriptor descriptor, int startIndex = 0, int limit = 72, QVariantMap queryOptions = {});
    QCoro::Task<QVariantMap> fetchLibraryFilterOptions(QString libraryId, QString collectionType = {});
    QCoro::Task<MovieItem> fetchItemDetails(QString itemId);
    QCoro::Task<std::vector<MovieItem>> fetchItemsByIds(QStringList itemIds);
    QCoro::Task<std::vector<MovieItem>> fetchSeasons(QString seriesId);
    QCoro::Task<std::vector<MovieItem>> fetchEpisodes(QString seriesId, QString seasonId = {});
    QCoro::Task<std::vector<MovieItem>> fetchResumeItems(int limit = 24);
    QCoro::Task<std::vector<MovieItem>> fetchNextUpEpisodes(int limit = 24);
    QCoro::Task<std::vector<MovieItem>> fetchLatestItems(QString parentId = {}, int limit = 24);
    QCoro::Task<std::vector<MovieItem>> searchItems(QString searchTerm, int limit = 80);
    QCoro::Task<std::vector<MovieItem>> fetchSearchSuggestions(int limit = 20);
    QCoro::Task<std::vector<MovieItem>> fetchSimilarItems(QString itemId, int limit = 24);
    QCoro::Task<PersonCredits> fetchItemsByPerson(QString personId, int maximumItems = 4000);
    QCoro::Task<std::vector<MovieItem>> fetchManagementTargets(QString itemType);
    QCoro::Task<QString> createPlaylist(QString name, QStringList itemIds = {});
    QCoro::Task<void> addPlaylistItems(QString playlistId, QStringList itemIds, int position = -1);
    QCoro::Task<void> removePlaylistItems(QString playlistId, QStringList entryIds);
    QCoro::Task<void> movePlaylistItem(QString playlistId, QString playlistItemId, int newIndex);
    QCoro::Task<void> updatePlaylistName(QString playlistId, QString name);
    QCoro::Task<QString> createCollection(QString name, QStringList itemIds = {});
    QCoro::Task<void> addCollectionItems(QString collectionId, QStringList itemIds);
    QCoro::Task<void> removeCollectionItems(QString collectionId, QStringList itemIds);
    QCoro::Task<void> renameItem(QString itemId, QString name);
    QCoro::Task<void> deleteItem(QString itemId);
    QCoro::Task<void> setItemFavorite(QString itemId, bool favorite);
    QCoro::Task<void> setItemPlayed(QString itemId, bool played);
    QCoro::Task<void> setItemPlaybackPosition(QString itemId, qint64 positionTicks);
    QCoro::Task<std::vector<MediaSegment>> fetchMediaSegments(QString itemId);
    QCoro::Task<TrickplayInfo> fetchTrickplayInfo(QString itemId, QString mediaSourceId = {});
    QString trickplayTileUrl(const QString& itemId, int width, int tileIndex) const;
    QCoro::Task<PlaybackSession> negotiatePlayback(MovieItem movie, bool forceTranscode = false);

    QCoro::Task<QJsonArray> fetchControllableSessions();
    QCoro::Task<void> sendRemotePlay(QString sessionId, QStringList itemIds, QString playCommand,
        qint64 startPositionTicks = -1, int startIndex = -1, QString mediaSourceId = {}, int audioStreamIndex = -2,
        int subtitleStreamIndex = -2);
    QCoro::Task<void> sendRemotePlaystate(QString sessionId, QString command, qint64 seekPositionTicks = -1);
    QCoro::Task<void> sendRemoteGeneralCommand(QString sessionId, QString command, QJsonObject arguments = {});

    // SyncPlay REST endpoints used alongside SyncPlayController's WebSocket.
    QCoro::Task<QJsonArray> fetchSyncPlayGroups();
    QCoro::Task<void> createSyncPlayGroup(QString name);
    QCoro::Task<void> joinSyncPlayGroup(QString groupId);
    QCoro::Task<void> leaveSyncPlayGroup();
    QCoro::Task<QJsonObject> fetchUtcTime();
    QCoro::Task<void> syncPlayReportPing(qint64 pingMs);
    QCoro::Task<void> syncPlayReportBuffering(
        bool buffering, qint64 positionTicks, bool playing, QString playlistItemId, QDateTime serverTime);
    QCoro::Task<void> syncPlaySetNewQueue(QStringList itemIds, int playingItemPosition, qint64 startPositionTicks);
    QCoro::Task<void> syncPlayPause();
    QCoro::Task<void> syncPlayUnpause();
    QCoro::Task<void> syncPlaySeek(qint64 positionTicks);
    QCoro::Task<void> syncPlayNextItem(QString playlistItemId);
    QCoro::Task<void> syncPlayPreviousItem(QString playlistItemId);
    // Group queue authoring. The queue is server state inside a group, so these
    // replace the local edits rather than accompanying them.
    QCoro::Task<void> syncPlayQueue(QStringList itemIds, bool queueNext);
    QCoro::Task<void> syncPlayMovePlaylistItem(QString playlistItemId, int newIndex);
    QCoro::Task<void> syncPlayRemoveFromPlaylist(QStringList playlistItemIds);
    QCoro::Task<void> syncPlaySetPlaylistItem(QString playlistItemId);

    QCoro::Task<void> postCapabilities();
    QCoro::Task<void> reportPlaybackStart(PlaybackSession session, double playbackRate, int volume, bool muted);
    QCoro::Task<void> reportPlaybackProgress(
        PlaybackSession session, qint64 positionTicks, bool paused, double playbackRate, int volume, bool muted);
    QCoro::Task<void> reportPlaybackStopped(
        PlaybackSession session, qint64 positionTicks, bool failed, double playbackRate = 1.0);

signals:
    void authenticationExpired(const QString& message);
    void deviceProfileChanged();
    void playbackNetworkProfileChanged();
    void streamingBitrateChanged();
    void sessionTokenChanged();

private:
    enum class HttpMethod {
        Get,
        Post,
        Delete,
    };

    QNetworkRequest createRequest(const QString& path, const QUrlQuery& query = {}) const;
    QCoro::Task<qint64> measurePlaybackBitrate(int totalSampleBytes, int parallelRequests);
    QCoro::Task<qint64> measurePlaybackRoundTripTime();
    QCoro::Task<QJsonDocument> requestJson(
        HttpMethod method, QString path, QUrlQuery query = {}, QJsonDocument body = {});
    QCoro::Task<void> requestNoContent(HttpMethod method, QString path, QJsonDocument body);
    QCoro::Task<QByteArray> requestBytes(
        HttpMethod method, QString path, QUrlQuery query = {}, QJsonDocument body = {});

    QJsonObject buildDeviceProfile() const;
    PlaybackSession buildPlaybackSession(const MovieItem& movie, const QJsonObject& playbackResponse) const;
    HttpOperation operationFor(HttpMethod method, const QString& path) const;
    bool shouldExpireSession(const QString& path) const;
    void preconnectToServer();
    void applyCommonHeaders();
    void updateEffectiveStreamingBitrate();
    void setPlaybackParallelRequests(int parallelRequests);
    QString currentNetworkSignature() const;
    void restoreRememberedMeasurement();
    void rememberMeasurement();
    void handleNetworkRouteChanged();

    QNetworkAccessManager *m_networkAccessManager = nullptr;
    QRestAccessManager m_rest;
    QNetworkRequestFactory m_requestFactory;
    QString m_serverUrl;
    QString m_deviceId;
    QString m_deviceName = QStringLiteral("LG webOS TV");
    QString m_clientVersion = QStringLiteral("0.1.0");
    AuthSession m_session;
    QSet<QNetworkReply *> m_activeReplies;
    QString m_preconnectedAuthority;
    QString m_acceptLanguage;
    qint64 m_maxStreamingBitrate = 20'000'000;
    qint64 m_manualMaxStreamingBitrate = 0;
    qint64 m_sessionBitrateOverride = 0;
    int m_maxStreamingHeight = 0;
    int m_sessionHeightOverride = 0;
    qint64 m_measuredStreamingBitrate = 0;
    QString m_measuredNetworkSignature;
    int m_playbackParallelRequests = 1;
    quint64 m_playbackNetworkGeneration = 0;
    bool m_playbackEndpointKnown = false;
    bool m_inLocalNetwork = false;
    PlaybackBandwidthPolicy::Source m_streamingBitrateSource = PlaybackBandwidthPolicy::Source::Estimate;
    bool m_measurementRemembered = false;
    bool m_playbackActive = false;
    bool m_measurementDeferred = false;
    bool m_unlimitedLocalNetwork = false;
    QStringList m_videoCodecs;
    bool m_restrictVideoCodecs = false;
    bool m_remoteControlTargetEnabled = true;
    bool m_preferRemux = true;
    bool m_authExpirationReported = false;
    bool m_shuttingDown = false;
};

} // namespace JellyfinNative
