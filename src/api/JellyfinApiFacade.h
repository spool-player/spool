#pragma once

#include "../media/MediaTypes.h"
#include "HttpRequestPolicy.h"
#include "JellyfinSession.h"

#include <QCoroTask>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequestFactory>
#include <QObject>
#include <QRestAccessManager>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrlQuery>

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
    void setDeviceName(const QString& deviceName);
    QString deviceId() const;

    void setAcceptLanguage(const QString& bcp47Tag);

    void setSession(const AuthSession& session);
    AuthSession session() const;
    bool signedIn() const
    {
        return !m_session.accessToken.isEmpty();
    }

    void setPlaybackPreferences(
        qint64 manualMaxStreamingBitrate, bool unlimitedLocalNetwork, bool preferRemux, int maxStreamingHeight = 0);
    void setRemoteControlTargetEnabled(bool enabled);
    bool remoteControlTargetEnabled() const
    {
        return m_remoteControlTargetEnabled;
    }

    QString authorizationHeader(const QString& tokenOverride = {}) const;
    void cancelRequests();

    // Authentication & User configuration
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

    // Library management
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
    QCoro::Task<std::vector<MovieItem>> fetchItemsByIds(QStringList itemIds);

    // Trickplay (used for remote control timeline)
    QCoro::Task<TrickplayInfo> fetchTrickplayInfo(QString itemId, QString mediaSourceId = {});
    QString trickplayTileUrl(const QString& itemId, int width, int tileIndex) const;

    // Remote playback & target sessions
    QCoro::Task<QJsonArray> fetchControllableSessions();
    QCoro::Task<void> sendRemotePlay(QString sessionId, QStringList itemIds, QString playCommand,
        qint64 startPositionTicks = -1, int startIndex = -1, QString mediaSourceId = {}, int audioStreamIndex = -2,
        int subtitleStreamIndex = -2);
    QCoro::Task<void> sendRemotePlaystate(QString sessionId, QString command, qint64 seekPositionTicks = -1);
    QCoro::Task<void> sendRemoteGeneralCommand(QString sessionId, QString command, QJsonObject arguments = {});

    // SyncPlay REST endpoints
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
    QCoro::Task<void> syncPlayQueue(QStringList itemIds, bool queueNext);
    QCoro::Task<void> syncPlayMovePlaylistItem(QString playlistItemId, int newIndex);
    QCoro::Task<void> syncPlayRemoveFromPlaylist(QStringList playlistItemIds);
    QCoro::Task<void> syncPlaySetPlaylistItem(QString playlistItemId);

    // Capabilities
    QCoro::Task<void> postCapabilities();

signals:
    void authenticationExpired(const QString& message);
    void credentialsChanged();
    void deviceProfileChanged();

private:
    enum class HttpMethod {
        Get,
        Post,
        Delete,
    };

    QNetworkRequest createRequest(const QString& path, const QUrlQuery& query = {}) const;
    QCoro::Task<QJsonDocument> requestJson(
        HttpMethod method, QString path, QUrlQuery query = {}, QJsonDocument body = {});
    QCoro::Task<void> requestNoContent(HttpMethod method, QString path, QJsonDocument body);
    QCoro::Task<QByteArray> requestBytes(
        HttpMethod method, QString path, QUrlQuery query = {}, QJsonDocument body = {});

    QJsonObject buildDeviceProfile() const;
    HttpOperation operationFor(HttpMethod method, const QString& path) const;
    bool shouldExpireSession(const QString& path) const;
    void preconnectToServer();
    void applyCommonHeaders();

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
    int m_maxStreamingHeight = 0;
    bool m_remoteControlTargetEnabled = true;
    bool m_authExpirationReported = false;
    bool m_shuttingDown = false;
};

} // namespace JellyfinNative
