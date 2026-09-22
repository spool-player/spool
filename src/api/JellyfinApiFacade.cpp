#include "JellyfinApiFacade.h"

#include "../common/AsyncTask.h"
#include "../common/TlsTrust.h"
#include "../diagnostics/Diagnostics.h"
#include "PlaybackNegotiation.h"

#include <QCoroFuture>
#include <QCoroNetworkReply>
#include <QCoroTimer>

#include <QDebug>
#include <QHttpHeaders>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace JellyfinNative {

namespace {
    constexpr int kConnectionCacheExpirySeconds = 15 * 60;

    QString requireString(const QJsonObject& object, const QString& key)
    {
        const QString value = object.value(key).toString();
        if (value.isEmpty())
            throw std::runtime_error(QStringLiteral("Missing field: %1").arg(key).toStdString());
        return value;
    }

    bool isQuickConnectPath(const QString& path)
    {
        return path.startsWith(QStringLiteral("/QuickConnect/"))
            || path == QStringLiteral("/Users/AuthenticateWithQuickConnect");
    }
} // namespace

JellyfinApiFacade::JellyfinApiFacade(
    QNetworkAccessManager *networkAccessManager, TlsTrustController *tlsTrust, QObject *parent)
    : QObject(parent)
    , m_networkAccessManager(networkAccessManager)
    , m_rest(networkAccessManager, this)
{
    if (tlsTrust)
        tlsTrust->attachNetworkAccessManager(m_networkAccessManager, QStringLiteral("Jellyfin API"));
    m_requestFactory.setTransferTimeout(std::chrono::milliseconds(HttpRequestPolicy::transferTimeoutMs()));
    m_requestFactory.setAttribute(
        QNetworkRequest::ConnectionCacheExpiryTimeoutSecondsAttribute, kConnectionCacheExpirySeconds);
    applyCommonHeaders();
}

JellyfinApiFacade::~JellyfinApiFacade()
{
    cancelRequests();
}

void JellyfinApiFacade::setServerUrl(const QString& serverUrl)
{
    QString normalized = serverUrl;
    while (normalized.endsWith(QLatin1Char('/')))
        normalized.chop(1);
    m_serverUrl = normalized;
    m_requestFactory.setBaseUrl(QUrl(m_serverUrl));
    preconnectToServer();
}

QString JellyfinApiFacade::serverUrl() const
{
    return m_serverUrl;
}

void JellyfinApiFacade::setAcceptLanguage(const QString& bcp47Tag)
{
    if (bcp47Tag.isEmpty())
        return;
    m_acceptLanguage = bcp47Tag;
    applyCommonHeaders();
}

void JellyfinApiFacade::setDeviceIdentity(
    const QString& deviceId, const QString& deviceName, const QString& clientVersion)
{
    m_deviceId = deviceId;
    m_deviceName = deviceName;
    m_clientVersion = clientVersion;
}

void JellyfinApiFacade::setDeviceId(const QString& deviceId)
{
    m_deviceId = deviceId;
}

void JellyfinApiFacade::setDeviceName(const QString& deviceName)
{
    const QString trimmed = deviceName.trimmed();
    if (trimmed.isEmpty() || trimmed == m_deviceName)
        return;
    m_deviceName = trimmed;
    applyCommonHeaders();
}

QString JellyfinApiFacade::deviceId() const
{
    return m_deviceId;
}

void JellyfinApiFacade::setSession(const AuthSession& session)
{
    const bool changed = m_session != session;
    m_session = session;
    if (changed) {
        m_authExpirationReported = false;
        applyCommonHeaders();
        emit credentialsChanged();
    }
}

AuthSession JellyfinApiFacade::session() const
{
    return m_session;
}

void JellyfinApiFacade::setPlaybackPreferences(
    qint64 /*manualMaxStreamingBitrate*/, bool /*unlimitedLocalNetwork*/, bool /*preferRemux*/, int maxStreamingHeight)
{
    const int normalizedHeight = maxStreamingHeight > 0 ? maxStreamingHeight : 0;
    if (m_maxStreamingHeight != normalizedHeight) {
        m_maxStreamingHeight = normalizedHeight;
        emit deviceProfileChanged();
    }
}

void JellyfinApiFacade::setRemoteControlTargetEnabled(bool enabled)
{
    if (m_remoteControlTargetEnabled == enabled)
        return;
    m_remoteControlTargetEnabled = enabled;
    emit deviceProfileChanged();
}

void JellyfinApiFacade::cancelRequests()
{
    m_shuttingDown = true;
    for (QNetworkReply *reply : m_activeReplies) {
        if (reply && reply->isRunning())
            reply->abort();
    }
    m_activeReplies.clear();
}

void JellyfinApiFacade::preconnectToServer()
{
    if (!m_networkAccessManager || m_serverUrl.isEmpty())
        return;
    const QUrl url(m_serverUrl);
    const QString authority = url.authority();
    if (authority.isEmpty() || authority == m_preconnectedAuthority)
        return;
    m_preconnectedAuthority = authority;
    const int port = url.port(url.scheme() == QStringLiteral("https") ? 443 : 80);
    m_networkAccessManager->connectToHost(url.host(), static_cast<quint16>(port));
}

QCoro::Task<AuthSession> JellyfinApiFacade::authenticateByName(QString username, QString password)
{
    const QJsonDocument response
        = co_await requestJson(HttpMethod::Post, QStringLiteral("/Users/AuthenticateByName"), {},
            QJsonDocument(QJsonObject {
                { QStringLiteral("Username"), username },
                { QStringLiteral("Pw"), password },
            }));

    const QJsonObject object = response.object();
    const QJsonObject user = object.value(QStringLiteral("User")).toObject();
    const AuthSession session {
        requireString(user, QStringLiteral("Id")),
        requireString(user, QStringLiteral("Name")),
        requireString(object, QStringLiteral("AccessToken")),
        object.value(QStringLiteral("ServerId")).toString(),
    };
    setSession(session);
    co_return session;
}

QCoro::Task<bool> JellyfinApiFacade::quickConnectEnabled()
{
    const QByteArray response = co_await requestBytes(HttpMethod::Get, QStringLiteral("/QuickConnect/Enabled"));
    co_return response.trimmed() == QByteArrayLiteral("true");
}

QCoro::Task<QJsonObject> JellyfinApiFacade::initiateQuickConnect()
{
    const QJsonDocument response = co_await requestJson(HttpMethod::Post, QStringLiteral("/QuickConnect/Initiate"));
    co_return response.object();
}

QCoro::Task<QJsonObject> JellyfinApiFacade::pollQuickConnect(QString secret)
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("secret"), secret);
    const QJsonDocument response
        = co_await requestJson(HttpMethod::Get, QStringLiteral("/QuickConnect/Connect"), query);
    co_return response.object();
}

QCoro::Task<AuthSession> JellyfinApiFacade::authenticateWithQuickConnect(QString secret)
{
    const QJsonDocument response
        = co_await requestJson(HttpMethod::Post, QStringLiteral("/Users/AuthenticateWithQuickConnect"), {},
            QJsonDocument(QJsonObject {
                { QStringLiteral("Secret"), secret },
            }));

    const QJsonObject object = response.object();
    const QJsonObject user = object.value(QStringLiteral("User")).toObject();
    const AuthSession session {
        requireString(user, QStringLiteral("Id")),
        requireString(user, QStringLiteral("Name")),
        requireString(object, QStringLiteral("AccessToken")),
        object.value(QStringLiteral("ServerId")).toString(),
    };
    setSession(session);
    co_return session;
}

QCoro::Task<QString> JellyfinApiFacade::fetchCurrentUserName()
{
    if (m_session.userId.isEmpty())
        co_return QString();

    const QJsonDocument response
        = co_await requestJson(HttpMethod::Get, QStringLiteral("/Users/%1").arg(m_session.userId));
    co_return response.object().value(QStringLiteral("Name")).toString();
}

QCoro::Task<QJsonObject> JellyfinApiFacade::fetchUserConfiguration()
{
    if (m_session.userId.isEmpty())
        co_return QJsonObject();

    const QJsonDocument response
        = co_await requestJson(HttpMethod::Get, QStringLiteral("/Users/%1").arg(m_session.userId));
    co_return response.object().value(QStringLiteral("Configuration")).toObject();
}

QCoro::Task<void> JellyfinApiFacade::updateUserConfiguration(QJsonObject configuration)
{
    if (m_session.userId.isEmpty())
        co_return;

    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/Users/%1/Configuration").arg(m_session.userId),
        QJsonDocument(configuration));
}

QCoro::Task<QJsonObject> JellyfinApiFacade::fetchCurrentUserPolicy()
{
    if (m_session.userId.isEmpty())
        co_return QJsonObject();

    const QJsonDocument response
        = co_await requestJson(HttpMethod::Get, QStringLiteral("/Users/%1").arg(m_session.userId));
    co_return response.object().value(QStringLiteral("Policy")).toObject();
}

QCoro::Task<QJsonArray> JellyfinApiFacade::fetchCultures()
{
    const QJsonDocument response = co_await requestJson(HttpMethod::Get, QStringLiteral("/Localization/Options"));
    co_return response.isArray() ? response.array() : response.object().value(QStringLiteral("Items")).toArray();
}

QCoro::Task<std::vector<MovieItem>> JellyfinApiFacade::fetchManagementTargets(QString itemType)
{
    if (m_serverUrl.isEmpty() || m_session.userId.isEmpty())
        co_return std::vector<MovieItem> {};

    itemType = itemType.trimmed();
    if (itemType != QStringLiteral("Playlist") && itemType != QStringLiteral("BoxSet"))
        co_return std::vector<MovieItem> {};

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("userId"), m_session.userId);
    query.addQueryItem(QStringLiteral("recursive"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("includeItemTypes"), itemType);
    query.addQueryItem(QStringLiteral("fields"), QStringLiteral("PrimaryImageAspectRatio,CanDelete"));
    query.addQueryItem(QStringLiteral("sortBy"), QStringLiteral("SortName"));
    query.addQueryItem(QStringLiteral("sortOrder"), QStringLiteral("Ascending"));
    query.addQueryItem(QStringLiteral("enableImageTypes"), QStringLiteral("Primary,Backdrop,Banner,Thumb"));
    query.addQueryItem(QStringLiteral("imageTypeLimit"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("200"));

    const QJsonArray items = (co_await requestJson(HttpMethod::Get, QStringLiteral("/Items"), query))
                                 .object()
                                 .value(QStringLiteral("Items"))
                                 .toArray();
    std::vector<MovieItem> result;
    result.reserve(items.size());
    for (const QJsonValue& val : items) {
        const QJsonObject obj = val.toObject();
        if (obj.value(QStringLiteral("Type")).toString() == itemType) {
            MovieItem item;
            item.id = obj.value(QStringLiteral("Id")).toString();
            item.title = obj.value(QStringLiteral("Name")).toString();
            item.itemType = itemType;
            result.push_back(std::move(item));
        }
    }
    co_return result;
}

QCoro::Task<QString> JellyfinApiFacade::createPlaylist(QString name, QStringList itemIds)
{
    name = name.trimmed();
    if (name.isEmpty())
        throw std::runtime_error("Playlist name is required");

    QJsonArray ids;
    for (const QString& itemId : itemIds) {
        if (!itemId.isEmpty())
            ids.push_back(itemId);
    }
    QJsonObject body {
        { QStringLiteral("Name"), name },
        { QStringLiteral("UserId"), m_session.userId },
        { QStringLiteral("Ids"), ids },
        { QStringLiteral("IsPublic"), false },
    };
    const QJsonObject response
        = (co_await requestJson(HttpMethod::Post, QStringLiteral("/Playlists"), {}, QJsonDocument(body))).object();
    co_return response.value(QStringLiteral("Id")).toString();
}

QCoro::Task<void> JellyfinApiFacade::addPlaylistItems(QString playlistId, QStringList itemIds, int position)
{
    if (playlistId.isEmpty() || itemIds.isEmpty())
        co_return;

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ids"), itemIds.join(QLatin1Char(',')));
    query.addQueryItem(QStringLiteral("userId"), m_session.userId);
    if (position >= 0)
        query.addQueryItem(QStringLiteral("position"), QString::number(position));
    co_await requestJson(HttpMethod::Post, QStringLiteral("/Playlists/%1/Items").arg(playlistId), query);
}

QCoro::Task<void> JellyfinApiFacade::removePlaylistItems(QString playlistId, QStringList entryIds)
{
    if (playlistId.isEmpty() || entryIds.isEmpty())
        co_return;

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("entryIds"), entryIds.join(QLatin1Char(',')));
    co_await requestJson(HttpMethod::Delete, QStringLiteral("/Playlists/%1/Items").arg(playlistId), query);
}

QCoro::Task<void> JellyfinApiFacade::movePlaylistItem(QString playlistId, QString playlistItemId, int newIndex)
{
    if (playlistId.isEmpty() || playlistItemId.isEmpty() || newIndex < 0)
        co_return;

    co_await requestNoContent(HttpMethod::Post,
        QStringLiteral("/Playlists/%1/Items/%2/Move/%3").arg(playlistId, playlistItemId, QString::number(newIndex)),
        QJsonDocument());
}

QCoro::Task<void> JellyfinApiFacade::updatePlaylistName(QString playlistId, QString name)
{
    name = name.trimmed();
    if (playlistId.isEmpty() || name.isEmpty())
        co_return;

    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/Playlists/%1").arg(playlistId),
        QJsonDocument(QJsonObject { { QStringLiteral("Name"), name } }));
}

QCoro::Task<QString> JellyfinApiFacade::createCollection(QString name, QStringList itemIds)
{
    name = name.trimmed();
    if (name.isEmpty())
        throw std::runtime_error("Collection name is required");

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("name"), name);
    if (!itemIds.isEmpty())
        query.addQueryItem(QStringLiteral("ids"), itemIds.join(QLatin1Char(',')));
    query.addQueryItem(QStringLiteral("isLocked"), QStringLiteral("false"));
    const QJsonObject response
        = (co_await requestJson(HttpMethod::Post, QStringLiteral("/Collections"), query)).object();
    co_return response.value(QStringLiteral("Id")).toString();
}

QCoro::Task<void> JellyfinApiFacade::addCollectionItems(QString collectionId, QStringList itemIds)
{
    if (collectionId.isEmpty() || itemIds.isEmpty())
        co_return;

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ids"), itemIds.join(QLatin1Char(',')));
    co_await requestJson(HttpMethod::Post, QStringLiteral("/Collections/%1/Items").arg(collectionId), query);
}

QCoro::Task<void> JellyfinApiFacade::removeCollectionItems(QString collectionId, QStringList itemIds)
{
    if (collectionId.isEmpty() || itemIds.isEmpty())
        co_return;

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ids"), itemIds.join(QLatin1Char(',')));
    co_await requestJson(HttpMethod::Delete, QStringLiteral("/Collections/%1/Items").arg(collectionId), query);
}

QCoro::Task<void> JellyfinApiFacade::renameItem(QString itemId, QString name)
{
    name = name.trimmed();
    if (itemId.isEmpty() || name.isEmpty())
        co_return;

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("userId"), m_session.userId);
    QJsonObject item = (co_await requestJson(HttpMethod::Get, QStringLiteral("/Items/%1").arg(itemId), query)).object();
    item.insert(QStringLiteral("Name"), name);
    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/Items/%1").arg(itemId), QJsonDocument(item));
}

QCoro::Task<void> JellyfinApiFacade::deleteItem(QString itemId)
{
    if (itemId.isEmpty())
        co_return;

    co_await requestNoContent(HttpMethod::Delete, QStringLiteral("/Items/%1").arg(itemId), QJsonDocument());
}

QCoro::Task<std::vector<MovieItem>> JellyfinApiFacade::fetchItemsByIds(QStringList itemIds)
{
    itemIds.removeAll(QString());
    if (itemIds.isEmpty() || m_session.userId.isEmpty())
        co_return std::vector<MovieItem> {};

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ids"), itemIds.join(QLatin1Char(',')));
    query.addQueryItem(QStringLiteral("userId"), m_session.userId);
    query.addQueryItem(QStringLiteral("fields"), QStringLiteral("PrimaryImageAspectRatio,CanDelete,RunTimeTicks"));
    const QJsonArray items = (co_await requestJson(HttpMethod::Get, QStringLiteral("/Items"), query))
                                 .object()
                                 .value(QStringLiteral("Items"))
                                 .toArray();
    std::vector<MovieItem> result;
    result.reserve(items.size());
    for (const QJsonValue& val : items) {
        const QJsonObject obj = val.toObject();
        MovieItem item;
        item.id = obj.value(QStringLiteral("Id")).toString();
        item.title = obj.value(QStringLiteral("Name")).toString();
        item.itemType = obj.value(QStringLiteral("Type")).toString();
        item.runtimeTicks = obj.value(QStringLiteral("RunTimeTicks")).toVariant().toLongLong();
        result.push_back(std::move(item));
    }
    co_return result;
}

QCoro::Task<TrickplayInfo> JellyfinApiFacade::fetchTrickplayInfo(QString itemId, QString mediaSourceId)
{
    if (itemId.isEmpty() || m_session.userId.isEmpty())
        co_return TrickplayInfo {};

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("fields"), QStringLiteral("Trickplay"));
    const QJsonObject item = (co_await requestJson(HttpMethod::Get,
                                  QStringLiteral("/Users/%1/Items/%2").arg(m_session.userId, itemId), query))
                                 .object();
    co_return PlaybackNegotiation::selectTrickplay(
        item.value(QStringLiteral("Trickplay")).toObject(), mediaSourceId, 320);
}

QString JellyfinApiFacade::trickplayTileUrl(const QString& itemId, int width, int tileIndex) const
{
    if (m_serverUrl.isEmpty() || itemId.isEmpty() || width <= 0 || tileIndex < 0)
        return {};

    QUrl url = serverUrlWithPath(m_serverUrl,
        { QStringLiteral("Videos"), itemId, QStringLiteral("Trickplay"), QString::number(width),
            QStringLiteral("%1.jpg").arg(tileIndex) });

    return url.toString(QUrl::FullyEncoded);
}

QCoro::Task<QJsonArray> JellyfinApiFacade::fetchControllableSessions()
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("controllableByUserId"), m_session.userId);
    const QJsonDocument document = co_await requestJson(HttpMethod::Get, QStringLiteral("/Sessions"), query);
    co_return document.isArray() ? document.array() : document.object().value(QStringLiteral("Items")).toArray();
}

QCoro::Task<void> JellyfinApiFacade::sendRemotePlay(QString sessionId, QStringList itemIds, QString playCommand,
    qint64 startPositionTicks, int startIndex, QString mediaSourceId, int audioStreamIndex, int subtitleStreamIndex)
{
    sessionId = sessionId.trimmed();
    itemIds.removeAll(QString());
    if (sessionId.isEmpty() || itemIds.isEmpty())
        throw std::runtime_error("Remote play needs a target and at least one item");

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("playCommand"), playCommand.isEmpty() ? QStringLiteral("PlayNow") : playCommand);
    query.addQueryItem(QStringLiteral("itemIds"), itemIds.join(QLatin1Char(',')));
    if (startPositionTicks >= 0)
        query.addQueryItem(QStringLiteral("startPositionTicks"), QString::number(startPositionTicks));
    if (startIndex >= 0)
        query.addQueryItem(QStringLiteral("startIndex"), QString::number(startIndex));
    if (!mediaSourceId.isEmpty())
        query.addQueryItem(QStringLiteral("mediaSourceId"), mediaSourceId);
    if (audioStreamIndex >= -1)
        query.addQueryItem(QStringLiteral("audioStreamIndex"), QString::number(audioStreamIndex));
    if (subtitleStreamIndex >= -1)
        query.addQueryItem(QStringLiteral("subtitleStreamIndex"), QString::number(subtitleStreamIndex));

    const QString encodedSession = QString::fromLatin1(QUrl::toPercentEncoding(sessionId));
    co_await requestBytes(
        HttpMethod::Post, QStringLiteral("/Sessions/%1/Playing").arg(encodedSession), query, QJsonDocument());
}

QCoro::Task<void> JellyfinApiFacade::sendRemotePlaystate(QString sessionId, QString command, qint64 seekPositionTicks)
{
    sessionId = sessionId.trimmed();
    if (sessionId.isEmpty() || command.isEmpty())
        throw std::runtime_error("Remote playstate needs a target and a command");

    QUrlQuery query;
    if (seekPositionTicks >= 0)
        query.addQueryItem(QStringLiteral("seekPositionTicks"), QString::number(seekPositionTicks));

    const QString encodedSession = QString::fromLatin1(QUrl::toPercentEncoding(sessionId));
    co_await requestBytes(HttpMethod::Post, QStringLiteral("/Sessions/%1/Playing/%2").arg(encodedSession, command),
        query, QJsonDocument());
}

QCoro::Task<void> JellyfinApiFacade::sendRemoteGeneralCommand(QString sessionId, QString command, QJsonObject arguments)
{
    sessionId = sessionId.trimmed();
    if (sessionId.isEmpty() || command.isEmpty())
        throw std::runtime_error("Remote command needs a target and a command name");

    const QJsonObject body = {
        { QStringLiteral("Name"), command },
        { QStringLiteral("Arguments"), arguments },
    };

    const QString encodedSession = QString::fromLatin1(QUrl::toPercentEncoding(sessionId));
    co_await requestBytes(
        HttpMethod::Post, QStringLiteral("/Sessions/%1/Command").arg(encodedSession), {}, QJsonDocument(body));
}

QCoro::Task<QJsonArray> JellyfinApiFacade::fetchSyncPlayGroups()
{
    const QJsonDocument document = co_await requestJson(HttpMethod::Get, QStringLiteral("/SyncPlay/List"));
    co_return document.isArray() ? document.array() : QJsonArray {};
}

QCoro::Task<void> JellyfinApiFacade::createSyncPlayGroup(QString name)
{
    const QJsonObject body = { { QStringLiteral("GroupName"), name } };
    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/SyncPlay/New"), QJsonDocument(body));
}

QCoro::Task<void> JellyfinApiFacade::joinSyncPlayGroup(QString groupId)
{
    const QJsonObject body = { { QStringLiteral("GroupId"), groupId } };
    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/SyncPlay/Join"), QJsonDocument(body));
}

QCoro::Task<void> JellyfinApiFacade::leaveSyncPlayGroup()
{
    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/SyncPlay/Leave"), QJsonDocument());
}

QCoro::Task<QJsonObject> JellyfinApiFacade::fetchUtcTime()
{
    const QJsonDocument response = co_await requestJson(HttpMethod::Get, QStringLiteral("/SyncPlay/Time"));
    co_return response.object();
}

QCoro::Task<void> JellyfinApiFacade::syncPlayReportPing(qint64 pingMs)
{
    const QJsonObject body = { { QStringLiteral("Ping"), pingMs } };
    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/SyncPlay/Ping"), QJsonDocument(body));
}

QCoro::Task<void> JellyfinApiFacade::syncPlayReportBuffering(
    bool buffering, qint64 positionTicks, bool playing, QString playlistItemId, QDateTime serverTime)
{
    if (playlistItemId.isEmpty())
        playlistItemId = QStringLiteral("00000000-0000-0000-0000-000000000000");

    const QJsonObject body = {
        { QStringLiteral("When"), serverTime.toUTC().toString(Qt::ISODateWithMs) },
        { QStringLiteral("PositionTicks"), positionTicks },
        { QStringLiteral("IsPlaying"), playing },
        { QStringLiteral("PlaylistItemId"), playlistItemId },
    };
    const QString path = buffering ? QStringLiteral("/SyncPlay/Buffering") : QStringLiteral("/SyncPlay/Ready");
    co_await requestNoContent(HttpMethod::Post, path, QJsonDocument(body));
}

QCoro::Task<void> JellyfinApiFacade::syncPlaySetNewQueue(
    QStringList itemIds, int playingItemPosition, qint64 startPositionTicks)
{
    QJsonArray playingQueue;
    for (const QString& itemId : itemIds) {
        if (!itemId.isEmpty())
            playingQueue.append(itemId);
    }
    if (playingQueue.isEmpty() || playingItemPosition < 0 || playingItemPosition >= playingQueue.size())
        throw std::runtime_error("SyncPlay queue has no playable item");

    const QJsonObject body = {
        { QStringLiteral("PlayingQueue"), playingQueue },
        { QStringLiteral("PlayingItemPosition"), playingItemPosition },
        { QStringLiteral("StartPositionTicks"), startPositionTicks },
    };
    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/SyncPlay/SetNewQueue"), QJsonDocument(body));
}

QCoro::Task<void> JellyfinApiFacade::syncPlayUnpause()
{
    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/SyncPlay/Unpause"), QJsonDocument());
}

QCoro::Task<void> JellyfinApiFacade::syncPlayPause()
{
    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/SyncPlay/Pause"), QJsonDocument());
}

QCoro::Task<void> JellyfinApiFacade::syncPlaySeek(qint64 positionTicks)
{
    const QJsonObject body = {
        { QStringLiteral("PositionTicks"), std::max<qint64>(0, positionTicks) },
    };
    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/SyncPlay/Seek"), QJsonDocument(body));
}

QCoro::Task<void> JellyfinApiFacade::syncPlayNextItem(QString playlistItemId)
{
    const QJsonObject body = { { QStringLiteral("PlaylistItemId"), playlistItemId } };
    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/SyncPlay/NextItem"), QJsonDocument(body));
}

QCoro::Task<void> JellyfinApiFacade::syncPlayPreviousItem(QString playlistItemId)
{
    const QJsonObject body = { { QStringLiteral("PlaylistItemId"), playlistItemId } };
    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/SyncPlay/PreviousItem"), QJsonDocument(body));
}

QCoro::Task<void> JellyfinApiFacade::syncPlayQueue(QStringList itemIds, bool queueNext)
{
    QJsonArray ids;
    for (const QString& itemId : itemIds) {
        if (!itemId.isEmpty())
            ids.append(itemId);
    }
    if (ids.isEmpty())
        throw std::runtime_error("SyncPlay queue request has no items");

    const QJsonObject body = {
        { QStringLiteral("ItemIds"), ids },
        { QStringLiteral("Mode"), queueNext ? QStringLiteral("QueueNext") : QStringLiteral("Queue") },
    };
    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/SyncPlay/Queue"), QJsonDocument(body));
}

QCoro::Task<void> JellyfinApiFacade::syncPlayMovePlaylistItem(QString playlistItemId, int newIndex)
{
    if (playlistItemId.isEmpty())
        throw std::runtime_error("SyncPlay move needs a playlist item id");

    const QJsonObject body = {
        { QStringLiteral("PlaylistItemId"), playlistItemId },
        { QStringLiteral("NewIndex"), std::max(0, newIndex) },
    };
    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/SyncPlay/MovePlaylistItem"), QJsonDocument(body));
}

QCoro::Task<void> JellyfinApiFacade::syncPlayRemoveFromPlaylist(QStringList playlistItemIds)
{
    QJsonArray ids;
    for (const QString& playlistItemId : playlistItemIds) {
        if (!playlistItemId.isEmpty())
            ids.append(playlistItemId);
    }
    if (ids.isEmpty())
        throw std::runtime_error("SyncPlay removal has no playlist items");

    const QJsonObject body = {
        { QStringLiteral("PlaylistItemIds"), ids },
        { QStringLiteral("ClearPlaylist"), false },
        { QStringLiteral("ClearPlayingItem"), false },
    };
    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/SyncPlay/RemoveFromPlaylist"), QJsonDocument(body));
}

QCoro::Task<void> JellyfinApiFacade::syncPlaySetPlaylistItem(QString playlistItemId)
{
    if (playlistItemId.isEmpty())
        throw std::runtime_error("SyncPlay jump needs a playlist item id");

    const QJsonObject body = { { QStringLiteral("PlaylistItemId"), playlistItemId } };
    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/SyncPlay/SetPlaylistItem"), QJsonDocument(body));
}

QCoro::Task<void> JellyfinApiFacade::postCapabilities()
{
    QJsonArray supportedCommands;
    if (m_remoteControlTargetEnabled) {
        supportedCommands = {
            QStringLiteral("MoveUp"),
            QStringLiteral("MoveDown"),
            QStringLiteral("MoveLeft"),
            QStringLiteral("MoveRight"),
            QStringLiteral("PageUp"),
            QStringLiteral("PageDown"),
            QStringLiteral("PreviousLetter"),
            QStringLiteral("NextLetter"),
            QStringLiteral("Select"),
            QStringLiteral("Back"),
            QStringLiteral("SendKey"),
            QStringLiteral("SendString"),
            QStringLiteral("VolumeUp"),
            QStringLiteral("VolumeDown"),
            QStringLiteral("Mute"),
            QStringLiteral("Unmute"),
            QStringLiteral("ToggleMute"),
            QStringLiteral("SetVolume"),
            QStringLiteral("SetAudioStreamIndex"),
            QStringLiteral("SetSubtitleStreamIndex"),
            QStringLiteral("ToggleOsd"),
            QStringLiteral("ToggleOsdMenu"),
            QStringLiteral("ToggleContextMenu"),
            QStringLiteral("ToggleStats"),
            QStringLiteral("ToggleFullscreen"),
            QStringLiteral("GoHome"),
            QStringLiteral("GoToSettings"),
            QStringLiteral("GoToSearch"),
            QStringLiteral("DisplayContent"),
            QStringLiteral("DisplayMessage"),
            QStringLiteral("SetRepeatMode"),
            QStringLiteral("SetShuffleQueue"),
            QStringLiteral("SetPlaybackOrder"),
            QStringLiteral("SetMaxStreamingBitrate"),
            QStringLiteral("Play"),
        };
    }
    const QJsonObject body = {
        { QStringLiteral("PlayableMediaTypes"),
            m_remoteControlTargetEnabled ? QJsonArray { QStringLiteral("Video"), QStringLiteral("Audio") }
                                         : QJsonArray {} },
        { QStringLiteral("SupportedCommands"), supportedCommands },
        { QStringLiteral("SupportsMediaControl"), m_remoteControlTargetEnabled },
        { QStringLiteral("SupportsPersistentIdentifier"), true },
        { QStringLiteral("DeviceProfile"), buildDeviceProfile() },
    };

    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/Sessions/Capabilities/Full"), QJsonDocument(body));
}

QJsonObject JellyfinApiFacade::buildDeviceProfile() const
{
    return PlaybackNegotiation::buildDeviceProfile(20'000'000, m_maxStreamingHeight);
}

QNetworkRequest JellyfinApiFacade::createRequest(const QString& path, const QUrlQuery& query) const
{
    QNetworkRequest request
        = query.isEmpty() ? m_requestFactory.createRequest(path) : m_requestFactory.createRequest(path, query);
    if (!HttpRequestPolicy::allowsCredentialTransport(request.url()))
        throw std::runtime_error("Credentials require HTTPS or a numeric private/loopback HTTP address");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Authorization", authorizationHeader().toUtf8());
    return request;
}

QString JellyfinApiFacade::authorizationHeader(const QString& tokenOverride) const
{
    QStringList parts {
        QStringLiteral("Client=\"Spool for Jellyfin\""),
        QStringLiteral("Device=\"%1\"").arg(m_deviceName),
        QStringLiteral("DeviceId=\"%1\"").arg(m_deviceId),
        QStringLiteral("Version=\"%1\"").arg(m_clientVersion),
    };

    const QString token = tokenOverride.isEmpty() ? m_session.accessToken : tokenOverride;
    if (!token.isEmpty())
        parts.push_back(QStringLiteral("Token=\"%1\"").arg(token));
    return QStringLiteral("MediaBrowser %1").arg(parts.join(QStringLiteral(", ")));
}

void JellyfinApiFacade::applyCommonHeaders()
{
    QHttpHeaders headers;
    headers.append(QHttpHeaders::WellKnownHeader::Accept, QStringLiteral("application/json"));
    if (!m_acceptLanguage.isEmpty())
        headers.append(QHttpHeaders::WellKnownHeader::AcceptLanguage, m_acceptLanguage);
    m_requestFactory.setCommonHeaders(headers);
}

QCoro::Task<QJsonDocument> JellyfinApiFacade::requestJson(
    HttpMethod method, QString path, QUrlQuery query, QJsonDocument body)
{
    const QByteArray payload = co_await requestBytes(method, path, query, body);
    if (payload.isEmpty())
        co_return QJsonDocument();

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        throw std::runtime_error(parseError.errorString().toStdString());
    co_return document;
}

QCoro::Task<void> JellyfinApiFacade::requestNoContent(HttpMethod method, QString path, QJsonDocument body)
{
    co_await requestBytes(method, path, {}, body);
}

QCoro::Task<QByteArray> JellyfinApiFacade::requestBytes(
    HttpMethod method, QString path, QUrlQuery query, QJsonDocument body)
{
    const QString methodName = method == HttpMethod::Get ? QStringLiteral("GET")
        : method == HttpMethod::Post                     ? QStringLiteral("POST")
                                                         : QStringLiteral("DELETE");
    const HttpOperation operation = operationFor(method, path);
    const int maximumAttempts = HttpRequestPolicy::maximumAttempts(operation);

    for (int attempt = 1; attempt <= maximumAttempts; ++attempt) {
        if (m_shuttingDown)
            throw std::runtime_error("Request canceled during shutdown");

        QNetworkRequest request = createRequest(path, query);
        if (method == HttpMethod::Post && body.isNull())
            request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        Diagnostics::NetworkRequest diagnosticsRequest(methodName, request.url().toString(QUrl::FullyEncoded));
        QNetworkReply *reply = nullptr;

        if (isQuickConnectPath(path))
            qInfo() << "api: quick connect request" << methodName;

        switch (method) {
        case HttpMethod::Get:
            reply = m_rest.get(request);
            break;
        case HttpMethod::Post:
            reply = body.isNull() ? m_rest.post(request, QByteArray {}) : m_rest.post(request, body);
            break;
        case HttpMethod::Delete:
            reply = m_rest.deleteResource(request);
            break;
        }

        m_activeReplies.insert(reply);
        reply = co_await reply;
        m_activeReplies.remove(reply);
        const QByteArray payload = reply && reply->isReadable() ? reply->readAll() : QByteArray {};
        const QString errorText = reply ? reply->errorString() : QStringLiteral("Network reply disappeared");
        const int statusCode = reply ? reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 500;
        const auto networkError = reply ? reply->error() : QNetworkReply::UnknownNetworkError;
        if (reply)
            reply->deleteLater();
        diagnosticsRequest.finish(statusCode, networkError == QNetworkReply::NoError ? QString() : errorText);

        if (networkError == QNetworkReply::NoError && statusCode < 300)
            co_return payload;

        QString details = payload.isEmpty() ? errorText : QString::fromUtf8(payload).trimmed();
        if (path == QStringLiteral("/Users/AuthenticateByName")) {
            const QJsonDocument errorDocument = QJsonDocument::fromJson(payload);
            if (errorDocument.isObject()) {
                const QJsonObject object = errorDocument.object();
                details = object.value(QStringLiteral("detail")).toString();
                if (details.isEmpty())
                    details = object.value(QStringLiteral("Message")).toString();
                if (details.isEmpty())
                    details = object.value(QStringLiteral("message")).toString();
                if (details.isEmpty())
                    details = object.value(QStringLiteral("ResponseStatus"))
                                  .toObject()
                                  .value(QStringLiteral("Message"))
                                  .toString();
                if (details.isEmpty())
                    details = object.value(QStringLiteral("title")).toString();
            }
            const bool generic = details.isEmpty() || details == errorText
                || details.startsWith(QStringLiteral("Error processing request"), Qt::CaseInsensitive)
                || details.compare(QStringLiteral("Unauthorized"), Qt::CaseInsensitive) == 0
                || details.startsWith(QLatin1Char('<'));
            if (generic) {
                if (statusCode == 401)
                    details = QStringLiteral("Incorrect username or password.");
                else if (statusCode == 403)
                    details
                        = QStringLiteral("Sign-in is not allowed for this account. Contact your server administrator.");
                else
                    details = errorText;
            }
        }
        if (statusCode == 401 && shouldExpireSession(path) && !m_authExpirationReported) {
            m_authExpirationReported = true;
            emit authenticationExpired(QStringLiteral("Your Jellyfin session has expired. Sign in again."));
        }

        if (!HttpRequestPolicy::shouldRetry(operation, attempt, statusCode, networkError)) {
            if (isQuickConnectPath(path))
                qWarning() << "api: quick connect request failed" << statusCode;
            throw std::runtime_error(QStringLiteral("%1 (%2)").arg(details).arg(statusCode).toStdString());
        }

        const int delayMs = HttpRequestPolicy::retryDelayMs(attempt);
        qWarning() << "api: request attempt" << attempt << "failed with" << statusCode << "- retrying in" << delayMs
                   << "ms";
        co_await QCoro::sleepFor(std::chrono::milliseconds(delayMs));
    }

    throw std::runtime_error("HTTP retry policy exhausted");
}

HttpOperation JellyfinApiFacade::operationFor(HttpMethod method, const QString& path) const
{
    if (path.startsWith(QStringLiteral("/Sessions/Playing")))
        return HttpOperation::PlaybackReport;
    return method == HttpMethod::Get ? HttpOperation::Read : HttpOperation::Mutation;
}

bool JellyfinApiFacade::shouldExpireSession(const QString& path) const
{
    return !m_session.accessToken.isEmpty() && !isQuickConnectPath(path)
        && !path.startsWith(QStringLiteral("/Users/Authenticate"));
}

} // namespace JellyfinNative
