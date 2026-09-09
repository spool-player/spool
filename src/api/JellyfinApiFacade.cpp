#include "JellyfinApiFacade.h"

#include "../common/AsyncTask.h"
#include "../common/MetaJson.h"
#include "../common/NetworkAddress.h"
#include "../common/TlsTrust.h"
#include "../common/VariantUtils.h"
#include "../diagnostics/Diagnostics.h"
#include "ItemsQuery.h"
#include "PlaybackBandwidthPolicy.h"
#include "PlaybackNegotiation.h"

#include <QCoroNetwork>
#include <QCoroTimer>

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QHttpHeaders>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkInformation>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QSettings>
#if QT_CONFIG(ssl)
#include <QSslConfiguration>
#endif
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
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

    QString cleanContainerName(const QString& container)
    {
        if (container.contains(QLatin1Char(',')))
            return container.section(QLatin1Char(','), 0, 0).trimmed();
        return container.trimmed();
    }

    bool isQuickConnectPath(const QString& path)
    {
        return path.startsWith(QStringLiteral("/QuickConnect/"))
            || path == QStringLiteral("/Users/AuthenticateWithQuickConnect");
    }

    QJsonArray itemsArrayFromDocument(const QJsonDocument& document)
    {
        if (document.isArray())
            return document.array();
        return document.object().value(QStringLiteral("Items")).toArray();
    }

    QString detailItemFields()
    {
        return QStringLiteral(
            "Overview,ProductionYear,PremiereDate,EndDate,Status,DateCreated,DateLastContentAdded,ImageTags,"
            "BackdropImageTags,"
            "UserData,Path,RunTimeTicks,SeriesInfo,LocationType,IsVirtualItem,Genres,Tags,Studios,ProviderIds,"
            "ExternalUrls,OfficialRating,CommunityRating,CriticRating,People,PrimaryImageAspectRatio,MediaSources");
    }

    QString libraryItemFields()
    {
        return QStringLiteral(
            "SortName,Overview,ProductionYear,PremiereDate,EndDate,Status,DateCreated,DateLastContentAdded,ImageTags,"
            "BackdropImageTags,UserData,RunTimeTicks,SeriesInfo,LocationType,IsVirtualItem,Genres,Tags,Studios,"
            "OfficialRating,CommunityRating,CriticRating,PrimaryImageAspectRatio");
    }

    QString firstString(const QJsonArray& array)
    {
        for (const QJsonValue& value : array) {
            const QString stringValue = value.toString();
            if (!stringValue.isEmpty())
                return stringValue;
        }
        return {};
    }

    QString includeItemTypesForCollection(QString collectionType)
    {
        if (collectionType == QStringLiteral("movies"))
            return QStringLiteral("Movie");
        if (collectionType == QStringLiteral("tvshows"))
            return QStringLiteral("Series");
        if (collectionType == QStringLiteral("playlists"))
            return QStringLiteral("Playlist");
        if (collectionType == QStringLiteral("boxsets"))
            return QStringLiteral("BoxSet");
        if (collectionType == QStringLiteral("music"))
            return QStringLiteral("MusicArtist,MusicAlbum,Audio");
        if (collectionType == QStringLiteral("books"))
            return QStringLiteral("Book,AudioBook");
        if (collectionType == QStringLiteral("photos"))
            return QStringLiteral("PhotoAlbum,Photo");
        if (collectionType == QStringLiteral("musicvideos"))
            return QStringLiteral("MusicVideo");
        if (collectionType == QStringLiteral("homevideos"))
            return QStringLiteral("Folder,Video,PhotoAlbum,Photo");
        return QStringLiteral("Folder,Movie,Series,Season,Episode,MusicVideo,Video,Audio,MusicAlbum,"
                              "MusicArtist,Book,AudioBook,Photo,PhotoAlbum,Playlist,BoxSet");
    }

    QStringList itemTypesList(const QString& types)
    {
        return types.split(QLatin1Char(','), Qt::SkipEmptyParts);
    }

    bool libraryBrowseUsesDirectChildren(const QString& collectionType)
    {
        return collectionType == QStringLiteral("playlists") || collectionType == QStringLiteral("boxsets");
    }

    void addJoinedQueryItem(QUrlQuery& query, const QVariantMap& options, const QString& key, const QString& queryKey,
        QLatin1Char delimiter)
    {
        const QStringList values = stringListFromVariantMap(options, key);
        if (!values.isEmpty())
            query.addQueryItem(queryKey, values.join(delimiter));
    }

    void addOptionalBoolQueryItem(
        QUrlQuery& query, const QVariantMap& options, const QString& key, const QString& queryKey)
    {
        if (!options.contains(key))
            return;
        const QVariant value = options.value(key);
        if (!value.isValid() || value.isNull())
            return;
        query.addQueryItem(queryKey, value.toBool() ? QStringLiteral("true") : QStringLiteral("false"));
    }

    void addLibraryQueryOptions(QUrlQuery& query, const QVariantMap& options)
    {
        const QString sortBy = options.value(QStringLiteral("sortBy"), QStringLiteral("SortName")).toString();
        const QString sortOrder = options.value(QStringLiteral("sortOrder"), QStringLiteral("Ascending")).toString();
        query.addQueryItem(QStringLiteral("sortBy"), sortBy.isEmpty() ? QStringLiteral("SortName") : sortBy);
        query.addQueryItem(QStringLiteral("sortOrder"), sortOrder.isEmpty() ? QStringLiteral("Ascending") : sortOrder);

        addJoinedQueryItem(query, options, QStringLiteral("filters"), QStringLiteral("filters"), QLatin1Char(','));
        addJoinedQueryItem(query, options, QStringLiteral("genres"), QStringLiteral("genres"), QLatin1Char('|'));
        addJoinedQueryItem(
            query, options, QStringLiteral("officialRatings"), QStringLiteral("officialRatings"), QLatin1Char('|'));
        addJoinedQueryItem(query, options, QStringLiteral("tags"), QStringLiteral("tags"), QLatin1Char('|'));
        addJoinedQueryItem(query, options, QStringLiteral("years"), QStringLiteral("years"), QLatin1Char(','));
        addJoinedQueryItem(query, options, QStringLiteral("studioIds"), QStringLiteral("studioIds"), QLatin1Char('|'));
        addJoinedQueryItem(
            query, options, QStringLiteral("seriesStatus"), QStringLiteral("seriesStatus"), QLatin1Char(','));
        addJoinedQueryItem(
            query, options, QStringLiteral("videoTypes"), QStringLiteral("videoTypes"), QLatin1Char(','));

        addOptionalBoolQueryItem(query, options, QStringLiteral("isHd"), QStringLiteral("isHd"));
        addOptionalBoolQueryItem(query, options, QStringLiteral("is4K"), QStringLiteral("is4K"));
        addOptionalBoolQueryItem(query, options, QStringLiteral("is3D"), QStringLiteral("is3D"));
        addOptionalBoolQueryItem(query, options, QStringLiteral("hasSubtitles"), QStringLiteral("hasSubtitles"));
        addOptionalBoolQueryItem(query, options, QStringLiteral("hasTrailer"), QStringLiteral("hasTrailer"));
        addOptionalBoolQueryItem(
            query, options, QStringLiteral("hasSpecialFeature"), QStringLiteral("hasSpecialFeature"));
        addOptionalBoolQueryItem(query, options, QStringLiteral("hasThemeSong"), QStringLiteral("hasThemeSong"));
        addOptionalBoolQueryItem(query, options, QStringLiteral("hasThemeVideo"), QStringLiteral("hasThemeVideo"));
        addOptionalBoolQueryItem(query, options, QStringLiteral("isMissing"), QStringLiteral("isMissing"));
        addOptionalBoolQueryItem(query, options, QStringLiteral("isUnaired"), QStringLiteral("isUnaired"));

        const QVariant specialEpisode = options.value(QStringLiteral("specialEpisode"));
        if (specialEpisode.isValid() && !specialEpisode.isNull() && specialEpisode.toBool())
            query.addQueryItem(QStringLiteral("parentIndexNumber"), QStringLiteral("0"));

        const QString alphabet = options.value(QStringLiteral("alphabet")).toString();
        if (!alphabet.isEmpty()) {
            if (alphabet == QStringLiteral("#"))
                query.addQueryItem(QStringLiteral("nameLessThan"), QStringLiteral("A"));
            else
                query.addQueryItem(QStringLiteral("nameStartsWith"), alphabet);
        }
    }

    QList<MediaStreamInfo> mediaStreamsFromApiJson(const QJsonArray& array)
    {
        QList<MediaStreamInfo> streams;
        streams.reserve(array.size());
        for (const QJsonValue& value : array) {
            const QJsonObject object = value.toObject();
            MediaStreamInfo stream = metaFromJson<MediaStreamInfo>(object, MetaJsonKeyPolicy::PascalCase);
            stream.frameRate = object.value(QStringLiteral("AverageFrameRate")).toDouble();
            if (stream.frameRate <= 0.0)
                stream.frameRate = object.value(QStringLiteral("RealFrameRate")).toDouble();
            if (!stream.type.isEmpty() || !stream.codec.isEmpty())
                streams.push_back(stream);
        }
        return streams;
    }

    MediaSourceInfo mediaSourceFromApiJson(const QJsonObject& object)
    {
        MediaSourceInfo source = metaFromJson<MediaSourceInfo>(object, MetaJsonKeyPolicy::PascalCase);
        source.container = cleanContainerName(object.value(QStringLiteral("Container")).toString());
        source.bitRate = object.value(QStringLiteral("Bitrate")).toInt();
        if (source.bitRate <= 0)
            source.bitRate = object.value(QStringLiteral("BitRate")).toInt();
        source.runtimeTicks = object.value(QStringLiteral("RunTimeTicks")).toVariant().toLongLong();
        source.streams = mediaStreamsFromApiJson(object.value(QStringLiteral("MediaStreams")).toArray());
        return source;
    }

    MovieItem mediaItemFromJson(const QJsonObject& object)
    {
        MovieItem item = metaFromJson<MovieItem>(object, MetaJsonKeyPolicy::PascalCase);
        item.posterTag
            = object.value(QStringLiteral("ImageTags")).toObject().value(QStringLiteral("Primary")).toString();
        item.logoTag = object.value(QStringLiteral("ImageTags")).toObject().value(QStringLiteral("Logo")).toString();
        item.bannerTag
            = object.value(QStringLiteral("ImageTags")).toObject().value(QStringLiteral("Banner")).toString();
        item.thumbTag = object.value(QStringLiteral("ImageTags")).toObject().value(QStringLiteral("Thumb")).toString();
        item.backdropTag = firstString(object.value(QStringLiteral("BackdropImageTags")).toArray());
        const int indexNumber = object.value(QStringLiteral("IndexNumber")).toInt();
        item.seasonNumber = item.itemType == QStringLiteral("Episode")
            ? object.value(QStringLiteral("ParentIndexNumber")).toInt()
            : indexNumber;
        item.episodeNumber = item.itemType == QStringLiteral("Episode") ? indexNumber : 0;
        const QJsonObject userData = object.value(QStringLiteral("UserData")).toObject();
        item.resumeTicks = normalizedResumeTicks(
            userData.value(QStringLiteral("PlaybackPositionTicks")).toVariant().toLongLong(), item.runtimeTicks);
        item.favorite = userData.value(QStringLiteral("IsFavorite")).toBool(false);
        item.played = userData.value(QStringLiteral("Played")).toBool(false);
        item.datePlayed = userData.value(QStringLiteral("LastPlayedDate")).toString();
        item.playCount = userData.value(QStringLiteral("PlayCount")).toInt();
        item.studios = metaStringListFromJson(object, { QStringLiteral("Studios"), QStringLiteral("Name") });
        item.mediaSources.clear();
        const QJsonArray sourceArray = object.value(QStringLiteral("MediaSources")).toArray();
        item.mediaSources.reserve(sourceArray.size());
        for (const QJsonValue& sourceValue : sourceArray) {
            MediaSourceInfo source = mediaSourceFromApiJson(sourceValue.toObject());
            if (!source.id.isEmpty() || !source.container.isEmpty() || !source.streams.isEmpty())
                item.mediaSources.push_back(source);
        }
        return item;
    }

    std::vector<MovieItem> mediaItemsFromJson(const QJsonArray& items, const QStringList& allowedTypes)
    {
        std::vector<MovieItem> result;
        result.reserve(items.size());
        for (const QJsonValue& value : items) {
            const QJsonObject object = value.toObject();
            if (allowedTypes.contains(object.value(QStringLiteral("Type")).toString()))
                result.push_back(mediaItemFromJson(object));
        }
        return result;
    }

}

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

    // Moving between Wi-Fi, ethernet and cellular changes what the link can
    // carry. Backends are optional: where none loads the ceiling simply keeps
    // whatever the last measurement found.
    if (QNetworkInformation::loadDefaultBackend()) {
        if (auto *information = QNetworkInformation::instance()) {
            connect(information, &QNetworkInformation::transportMediumChanged, this,
                &JellyfinApiFacade::handleNetworkRouteChanged);
            connect(information, &QNetworkInformation::reachabilityChanged, this,
                [this](QNetworkInformation::Reachability reachability) {
                    if (reachability == QNetworkInformation::Reachability::Online)
                        handleNetworkRouteChanged();
                });
        }
    }

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
    const bool serverChanged = normalized != m_serverUrl;
    m_serverUrl = normalized;
    m_requestFactory.setBaseUrl(QUrl(m_serverUrl));
    if (serverChanged) {
        ++m_playbackNetworkGeneration;
        m_inLocalNetwork = isLanHost(QUrl(m_serverUrl).host());
        m_playbackEndpointKnown = m_inLocalNetwork;
        m_measuredStreamingBitrate = 0;
        setPlaybackParallelRequests(1);
        updateEffectiveStreamingBitrate();
    }
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
    const bool tokenChanged = m_session.accessToken != session.accessToken;
    if (tokenChanged) {
        ++m_playbackNetworkGeneration;
        setPlaybackParallelRequests(1);
    }
    m_session = session;
    m_authExpirationReported = false;
    if (m_session.accessToken.isEmpty())
        m_preconnectedAuthority.clear();
    if (tokenChanged) {
        // The bitrate probe needs a token, so nothing can be measured before
        // sign-in. Reusing what this route measured last time gives playback a
        // real number at once instead of the conservative estimate, and the
        // probe that follows refreshes it.
        if (!m_session.accessToken.isEmpty())
            restoreRememberedMeasurement();
        emit sessionTokenChanged();
    }
    preconnectToServer();
}

void JellyfinApiFacade::preconnectToServer()
{
    if (!m_networkAccessManager || m_serverUrl.isEmpty() || m_session.accessToken.isEmpty())
        return;

    const QUrl url(m_serverUrl);
    if (!url.isValid() || url.host().isEmpty() || url.scheme() != QStringLiteral("https"))
        return;

    const int port = url.port(443);
    const QString authority = url.host() + QLatin1Char(':') + QString::number(port);
    if (m_preconnectedAuthority == authority)
        return;

    m_preconnectedAuthority = authority;
#if QT_CONFIG(ssl)
    m_networkAccessManager->connectToHostEncrypted(
        url.host(), static_cast<quint16>(port), QSslConfiguration::defaultConfiguration());
#else
    // The webOS Qt build has OpenSSL disabled; still warm DNS/TCP so the first
    // authenticated HTTPS request does not pay every socket setup cost.
    m_networkAccessManager->connectToHost(url.host(), static_cast<quint16>(port));
#endif
}

void JellyfinApiFacade::setPlaybackPreferences(
    qint64 manualMaxStreamingBitrate, bool unlimitedLocalNetwork, bool preferRemux, int maxStreamingHeight)
{
    m_manualMaxStreamingBitrate = manualMaxStreamingBitrate > 0
        ? std::clamp<qint64>(manualMaxStreamingBitrate, 1'000'000, PlaybackBandwidthPolicy::MaximumBitrate)
        : 0;
    m_unlimitedLocalNetwork = unlimitedLocalNetwork;
    m_preferRemux = preferRemux;
    const int normalizedHeight = maxStreamingHeight > 0 ? maxStreamingHeight : 0;
    if (m_maxStreamingHeight != normalizedHeight) {
        m_maxStreamingHeight = normalizedHeight;
        // The profile is what carries the ceiling, so a changed ceiling has to
        // reach anything holding one.
        emit deviceProfileChanged();
    }
    updateEffectiveStreamingBitrate();
}

void JellyfinApiFacade::setVideoCodecCapabilities(QStringList videoCodecs, bool restrictVideoCodecs)
{
    for (QString& codec : videoCodecs)
        codec = codec.trimmed().toLower();
    videoCodecs.removeAll(QString());
    videoCodecs.removeDuplicates();
    if (m_videoCodecs == videoCodecs && m_restrictVideoCodecs == restrictVideoCodecs)
        return;

    m_videoCodecs = std::move(videoCodecs);
    m_restrictVideoCodecs = restrictVideoCodecs;
    qInfo() << "playback capabilities: video codecs=" << m_videoCodecs << "restricted=" << m_restrictVideoCodecs;
    emit deviceProfileChanged();
}
void JellyfinApiFacade::setRemoteControlTargetEnabled(bool enabled)
{
    if (m_remoteControlTargetEnabled == enabled)
        return;
    m_remoteControlTargetEnabled = enabled;
    emit deviceProfileChanged();
}

int JellyfinApiFacade::playbackParallelRequests() const
{
    return m_playbackParallelRequests;
}

void JellyfinApiFacade::setPlaybackParallelRequests(int parallelRequests)
{
    const int normalized = std::clamp(parallelRequests, 1, 4);
    if (normalized == m_playbackParallelRequests)
        return;
    m_playbackParallelRequests = normalized;
    emit playbackNetworkProfileChanged();
}

qint64 JellyfinApiFacade::maxStreamingBitrate() const
{
    return m_maxStreamingBitrate;
}

qint64 JellyfinApiFacade::measuredStreamingBitrate() const
{
    return m_measuredStreamingBitrate;
}

void JellyfinApiFacade::setSessionBitrateOverride(qint64 bitrate)
{
    const qint64 normalized = bitrate > 0
        ? std::clamp<qint64>(bitrate, PlaybackBandwidthPolicy::MinimumBitrate, PlaybackBandwidthPolicy::MaximumBitrate)
        : 0;
    if (normalized == m_sessionBitrateOverride)
        return;
    m_sessionBitrateOverride = normalized;
    updateEffectiveStreamingBitrate();
}

qint64 JellyfinApiFacade::sessionBitrateOverride() const
{
    return m_sessionBitrateOverride;
}

void JellyfinApiFacade::setSessionHeightOverride(int height)
{
    const int normalized = height > 0 ? height : 0;
    if (normalized == m_sessionHeightOverride)
        return;
    m_sessionHeightOverride = normalized;
    emit deviceProfileChanged();
}

int JellyfinApiFacade::sessionHeightOverride() const
{
    return m_sessionHeightOverride;
}

int JellyfinApiFacade::maxStreamingHeight() const
{
    // The session override wins outright rather than taking the lower of the
    // two: a viewer who asks the overlay for 1080p on a device configured to
    // 480p is asking for 1080p now.
    return m_sessionHeightOverride > 0 ? m_sessionHeightOverride : m_maxStreamingHeight;
}

PlaybackBandwidthPolicy::Source JellyfinApiFacade::streamingBitrateSource() const
{
    if (m_sessionBitrateOverride > 0)
        return PlaybackBandwidthPolicy::Source::Manual;
    const PlaybackBandwidthPolicy::Source source
        = PlaybackBandwidthPolicy::effectiveBitrateSource(m_manualMaxStreamingBitrate, m_unlimitedLocalNetwork,
            m_playbackEndpointKnown, m_inLocalNetwork, m_measuredStreamingBitrate);
    // The policy cannot tell a measurement taken now from one restored for
    // this route, so name the difference here.
    if (source == PlaybackBandwidthPolicy::Source::Measured && m_measurementRemembered)
        return PlaybackBandwidthPolicy::Source::Remembered;
    return source;
}

void JellyfinApiFacade::setPlaybackActive(bool active)
{
    if (m_playbackActive == active)
        return;
    m_playbackActive = active;
    if (active || !m_measurementDeferred)
        return;
    m_measurementDeferred = false;
    Async::runScoped(
        this, refreshPlaybackNetworkState(), []() {},
        [](const std::exception_ptr& error) {
            qWarning() << "playback bandwidth: deferred measurement failed" << exceptionMessage(error);
        });
}

QString JellyfinApiFacade::currentNetworkSignature() const
{
    QStringList addresses;
    const QList<QHostAddress> local = QNetworkInterface::allAddresses();
    addresses.reserve(local.size());
    for (const QHostAddress& address : local)
        addresses.push_back(address.toString());

    QString transport;
    if (auto *information = QNetworkInformation::instance())
        transport = QVariant::fromValue(information->transportMedium()).toString();

    const QUrl url(m_serverUrl);
    const QString authority = url.host().toLower() + QLatin1Char(':') + QString::number(url.port(443));
    return PlaybackBandwidthPolicy::networkSignature(authority, addresses, transport);
}

void JellyfinApiFacade::restoreRememberedMeasurement()
{
    if (m_serverUrl.isEmpty())
        return;
    m_measuredNetworkSignature = currentNetworkSignature();
    m_measuredStreamingBitrate = 0;
    m_measurementRemembered = false;

    QSettings settings;
    settings.beginGroup(QStringLiteral("playback/bandwidth/") + m_measuredNetworkSignature);
    const qint64 bitrate = settings.value(QStringLiteral("bitrate")).toLongLong();
    const qint64 recordedAt = settings.value(QStringLiteral("recordedAt")).toLongLong();
    const int parallelRequests = settings.value(QStringLiteral("parallelRequests"), 1).toInt();
    settings.endGroup();

    if (bitrate <= 0
        || !PlaybackBandwidthPolicy::isRememberedMeasurementUsable(recordedAt, QDateTime::currentMSecsSinceEpoch())) {
        updateEffectiveStreamingBitrate();
        return;
    }

    m_measuredStreamingBitrate = bitrate;
    m_measurementRemembered = true;
    setPlaybackParallelRequests(parallelRequests);
    qInfo() << "playback bandwidth: restored" << bitrate << "for route" << m_measuredNetworkSignature;
    updateEffectiveStreamingBitrate();
}

void JellyfinApiFacade::rememberMeasurement()
{
    if (m_measuredStreamingBitrate <= 0 || m_measuredNetworkSignature.isEmpty())
        return;
    QSettings settings;
    settings.beginGroup(QStringLiteral("playback/bandwidth/") + m_measuredNetworkSignature);
    settings.setValue(QStringLiteral("bitrate"), m_measuredStreamingBitrate);
    settings.setValue(QStringLiteral("parallelRequests"), m_playbackParallelRequests);
    settings.setValue(QStringLiteral("recordedAt"), QDateTime::currentMSecsSinceEpoch());
    settings.endGroup();
}

void JellyfinApiFacade::handleNetworkRouteChanged()
{
    if (m_serverUrl.isEmpty() || m_session.accessToken.isEmpty())
        return;
    if (currentNetworkSignature() == m_measuredNetworkSignature)
        return;

    // A different route invalidates the measurement outright rather than
    // carrying a home Wi-Fi ceiling onto a phone hotspot. Reload whatever is
    // remembered for the new route so playback has a number immediately, then
    // re-measure unless a stream is already running.
    ++m_playbackNetworkGeneration;
    m_playbackEndpointKnown = false;
    m_inLocalNetwork = false;
    restoreRememberedMeasurement();
    Async::runScoped(
        this, refreshPlaybackNetworkState(), []() {},
        [](const std::exception_ptr& error) {
            qWarning() << "playback bandwidth: re-measurement failed" << exceptionMessage(error);
        });
}

void JellyfinApiFacade::updateEffectiveStreamingBitrate()
{
    // A ceiling picked in the player is a direct instruction, so it bypasses
    // every automatic input rather than competing with them.
    const qint64 effective = m_sessionBitrateOverride > 0
        ? m_sessionBitrateOverride
        : PlaybackBandwidthPolicy::effectiveBitrate(m_manualMaxStreamingBitrate, m_unlimitedLocalNetwork,
              m_playbackEndpointKnown, m_inLocalNetwork, m_measuredStreamingBitrate);
    const PlaybackBandwidthPolicy::Source source = streamingBitrateSource();
    // The description can change while the number does not: a remembered
    // ceiling confirmed by a fresh probe is the same ceiling, differently
    // earned, and the player says so.
    const bool descriptionChanged = source != m_streamingBitrateSource;
    m_streamingBitrateSource = source;
    if (effective == m_maxStreamingBitrate) {
        if (descriptionChanged)
            emit streamingBitrateChanged();
        return;
    }
    m_maxStreamingBitrate = effective;
    qInfo() << "playback bandwidth: effective=" << m_maxStreamingBitrate << "manual=" << m_manualMaxStreamingBitrate
            << "measured=" << m_measuredStreamingBitrate << "remembered=" << m_measurementRemembered
            << "endpointKnown=" << m_playbackEndpointKnown << "local=" << m_inLocalNetwork
            << "unlimitedLocal=" << m_unlimitedLocalNetwork;
    emit deviceProfileChanged();
    emit streamingBitrateChanged();
}

QCoro::Task<qint64> JellyfinApiFacade::measurePlaybackBitrate(int totalSampleBytes, int parallelRequests)
{
    parallelRequests = std::clamp(parallelRequests, 1, 4);
    const int bytesPerRequest = std::max(1, totalSampleBytes / parallelRequests);
    const qint64 cacheBuster = QDateTime::currentMSecsSinceEpoch();
    QElapsedTimer timer;
    timer.start();

    // QCoro::Task starts eagerly. Construct all requests before awaiting any
    // result so this measures aggregate throughput at the requested level of
    // concurrency, while keeping the total transferred bytes constant.
    std::vector<QCoro::Task<QByteArray>> tasks;
    tasks.reserve(static_cast<size_t>(parallelRequests));
    for (int lane = 0; lane < parallelRequests; ++lane) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("size"), QString::number(bytesPerRequest));
        query.addQueryItem(QStringLiteral("_"), QStringLiteral("%1-%2").arg(cacheBuster).arg(lane));
        tasks.push_back(requestBytes(HttpMethod::Get, QStringLiteral("/Playback/BitrateTest"), query));
    }

    qint64 transferredBytes = 0;
    for (auto& task : tasks)
        transferredBytes += (co_await task).size();
    co_return PlaybackBandwidthPolicy::conservativeEstimate(transferredBytes, std::max<qint64>(1, timer.elapsed()));
}

QCoro::Task<qint64> JellyfinApiFacade::measurePlaybackRoundTripTime()
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("_"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    QElapsedTimer timer;
    timer.start();
    co_await requestBytes(HttpMethod::Get, QStringLiteral("/System/Endpoint"), query);
    co_return std::max<qint64>(1, timer.elapsed());
}

QCoro::Task<void> JellyfinApiFacade::refreshPlaybackNetworkState()
{
    if (m_serverUrl.isEmpty() || m_session.accessToken.isEmpty())
        co_return;
    if (m_playbackActive) {
        // Several megabytes of probe traffic would compete with the stream it
        // is meant to size. Whatever ceiling is already known carries this
        // playback; measure once the stream ends.
        m_measurementDeferred = true;
        co_return;
    }

    const quint64 generation = ++m_playbackNetworkGeneration;
    const QJsonObject endpoint = (co_await requestJson(HttpMethod::Get, QStringLiteral("/System/Endpoint"))).object();
    if (generation != m_playbackNetworkGeneration)
        co_return;

    m_playbackEndpointKnown = true;
    m_inLocalNetwork
        = endpoint.value(QStringLiteral("IsLocal")).toBool() || endpoint.value(QStringLiteral("IsInNetwork")).toBool();
    updateEffectiveStreamingBitrate();

    // Warm the authenticated server route and exclude DNS/TLS/request setup
    // from the samples used to select a streaming ceiling.
    co_await measurePlaybackBitrate(512 * 1024, 1);
    if (generation != m_playbackNetworkGeneration)
        co_return;
    const qint64 roundTripMilliseconds = co_await measurePlaybackRoundTripTime();
    if (generation != m_playbackNetworkGeneration)
        co_return;
    constexpr int benchmarkBytes = 4 * 1024 * 1024;
    const qint64 singleRequest = co_await measurePlaybackBitrate(benchmarkBytes, 1);
    if (generation != m_playbackNetworkGeneration)
        co_return;
    const qint64 dualRequest = co_await measurePlaybackBitrate(benchmarkBytes, 2);
    if (generation != m_playbackNetworkGeneration)
        co_return;

    qint64 fourRequest = 0;
    if (PlaybackBandwidthPolicy::shouldBenchmarkFourRequests(roundTripMilliseconds, singleRequest, dualRequest)) {
        fourRequest = co_await measurePlaybackBitrate(benchmarkBytes, 4);
        if (generation != m_playbackNetworkGeneration)
            co_return;
    }

    m_measuredStreamingBitrate = std::max({ singleRequest, dualRequest, fourRequest });
    m_measurementRemembered = false;
    m_measuredNetworkSignature = currentNetworkSignature();
    setPlaybackParallelRequests(
        PlaybackBandwidthPolicy::selectParallelRequests(singleRequest, dualRequest, fourRequest));
    qInfo() << "playback bandwidth: benchmark rtt_ms=" << roundTripMilliseconds << "one=" << singleRequest
            << "two=" << dualRequest << "four=" << fourRequest << "selectedRequests=" << m_playbackParallelRequests
            << "selectedBitrate=" << m_measuredStreamingBitrate;
    rememberMeasurement();
    updateEffectiveStreamingBitrate();
}

AuthSession JellyfinApiFacade::session() const
{
    return m_session;
}

void JellyfinApiFacade::cancelRequests()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    const auto replies = m_activeReplies;
    for (QNetworkReply *reply : replies) {
        if (reply)
            reply->abort();
    }
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

QCoro::Task<std::vector<LibraryItem>> JellyfinApiFacade::fetchLibraries()
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("userId"), m_session.userId);
    query.addQueryItem(QStringLiteral("includeHidden"), QStringLiteral("false"));
    query.addQueryItem(QStringLiteral("presetViews"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("enableImageTypes"), QStringLiteral("Primary"));
    query.addQueryItem(QStringLiteral("imageTypeLimit"), QStringLiteral("1"));

    const QJsonArray items = (co_await requestJson(HttpMethod::Get, QStringLiteral("/UserViews"), query))
                                 .object()
                                 .value(QStringLiteral("Items"))
                                 .toArray();

    std::vector<LibraryItem> libraries;
    libraries.reserve(items.size());
    for (const QJsonValue& value : items) {
        const QJsonObject object = value.toObject();
        LibraryItem library = metaFromJson<LibraryItem>(object, MetaJsonKeyPolicy::PascalCase);
        library.imageTag
            = object.value(QStringLiteral("ImageTags")).toObject().value(QStringLiteral("Primary")).toString();
        libraries.push_back(std::move(library));
    }

    co_return libraries;
}

QCoro::Task<PagedMovieItems> JellyfinApiFacade::fetchBrowsePage(
    BrowseDescriptor descriptor, int startIndex, int limit, QVariantMap queryOptions)
{
    if (!descriptor.isValid())
        co_return PagedMovieItems { {}, 0, std::max(0, startIndex), std::clamp(limit, 1, 200) };

    QString path = QStringLiteral("/Items");
    QStringList allowedTypes;
    const int maximumLimit = descriptor.kind == BrowseKind::Library ? 100 : 200;
    // Stream metadata feeds the browse info line; person credits page through
    // entire filmographies, so keep that path on the lean field set.
    const QString fields = descriptor.kind == BrowseKind::Person
        ? libraryItemFields()
        : libraryItemFields() + QStringLiteral(",MediaSources");
    ItemsQuery builder = ItemsQuery()
                             .userId(m_session.userId)
                             .fields(fields)
                             .images()
                             .startIndex(startIndex)
                             .limit(limit, maximumLimit);

    switch (descriptor.kind) {
    case BrowseKind::Library: {
        QString includeItemTypes = includeItemTypesForCollection(descriptor.collectionType);
        const QString requestedTypes
            = stringListFromVariantMap(queryOptions, QStringLiteral("includeItemTypes")).join(QLatin1Char(','));
        const bool collectionFilter
            = descriptor.collectionType == QStringLiteral("movies") && requestedTypes == QStringLiteral("BoxSet");
        if (collectionFilter)
            includeItemTypes = requestedTypes;
        builder.parentId(descriptor.id)
            .recursive(!collectionFilter && !libraryBrowseUsesDirectChildren(descriptor.collectionType))
            .includeItemTypes(includeItemTypes);
        allowedTypes = itemTypesList(includeItemTypes);
        break;
    }
    case BrowseKind::FolderChildren:
        builder.parentId(descriptor.id)
            .recursive(false)
            .includeItemTypes(
                QStringLiteral("Folder,Movie,Series,Season,Episode,MusicVideo,Video,Audio,AudioBook,Book,Photo,"
                               "PhotoAlbum,Playlist,BoxSet,MusicAlbum,MusicArtist"));
        allowedTypes = itemTypesList(
            QStringLiteral("Folder,Movie,Series,Season,Episode,MusicVideo,Video,Audio,AudioBook,Book,Photo,"
                           "PhotoAlbum,Playlist,BoxSet,MusicAlbum,MusicArtist"));
        break;
    case BrowseKind::Person:
        builder.recursive()
            .add(QStringLiteral("personIds"), descriptor.id)
            .includeItemTypes(QStringLiteral("Movie,Series,Episode"))
            .sort(QStringLiteral("SortName"));
        allowedTypes = itemTypesList(QStringLiteral("Movie,Series,Episode"));
        break;
    case BrowseKind::Genre: {
        const bool music = descriptor.collectionType == QStringLiteral("music");
        const QString types = music ? QStringLiteral("MusicAlbum,Audio") : QStringLiteral("Movie,Series");
        builder.recursive()
            .add(QStringLiteral("genres"), descriptor.name)
            .includeItemTypes(types)
            .sort(QStringLiteral("SortName"));
        if (!music)
            builder.mediaTypes(QStringLiteral("Video"));
        allowedTypes = itemTypesList(types);
        break;
    }
    case BrowseKind::Studio:
        builder.recursive()
            .add(QStringLiteral("studios"), descriptor.name)
            .includeItemTypes(QStringLiteral("Movie,Series"))
            .mediaTypes(QStringLiteral("Video"))
            .sort(QStringLiteral("SortName"));
        allowedTypes = itemTypesList(QStringLiteral("Movie,Series"));
        break;
    case BrowseKind::SeriesSeasons:
        path = QStringLiteral("/Shows/%1/Seasons")
                   .arg(descriptor.seriesId.isEmpty() ? descriptor.id : descriptor.seriesId);
        allowedTypes = itemTypesList(QStringLiteral("Season"));
        break;
    case BrowseKind::SeasonEpisodes:
        path = QStringLiteral("/Shows/%1/Episodes").arg(descriptor.seriesId);
        builder.addIfNotEmpty(QStringLiteral("seasonId"), descriptor.seasonId);
        allowedTypes = itemTypesList(QStringLiteral("Episode"));
        break;
    case BrowseKind::Playlist:
        path = QStringLiteral("/Playlists/%1/Items").arg(descriptor.id);
        allowedTypes = itemTypesList(QStringLiteral("Movie,Episode,MusicVideo,Video,Audio"));
        break;
    case BrowseKind::BoxSet:
        builder.parentId(descriptor.id)
            .recursive(false)
            .includeItemTypes(QStringLiteral("Movie,Series,Episode"))
            .mediaTypes(QStringLiteral("Video"))
            .sort(QStringLiteral("SortName"));
        allowedTypes = itemTypesList(QStringLiteral("Movie,Series,Episode"));
        break;
    case BrowseKind::ArtistAlbums:
        builder.recursive()
            .add(QStringLiteral("artistIds"), descriptor.id)
            .includeItemTypes(QStringLiteral("MusicAlbum"))
            .sort(QStringLiteral("SortName"));
        allowedTypes = itemTypesList(QStringLiteral("MusicAlbum"));
        break;
    case BrowseKind::None:
        break;
    }

    QUrlQuery query = builder.toUrlQuery();
    if (descriptor.kind == BrowseKind::Library)
        addLibraryQueryOptions(query, queryOptions);

    const QString pageName = descriptor.name.isEmpty() ? descriptor.id : descriptor.name;
    QElapsedTimer requestTimer;
    requestTimer.start();
    QJsonObject response;
    try {
        response = (co_await requestJson(HttpMethod::Get, path, query)).object();
    } catch (...) {
        qWarning() << "browse page: request failed" << pageName << "start=" << std::max(0, startIndex)
                   << "limit=" << std::clamp(limit, 1, maximumLimit) << "ms=" << requestTimer.elapsed();
        throw;
    }

    std::vector<MovieItem> items = mediaItemsFromJson(response.value(QStringLiteral("Items")).toArray(), allowedTypes);
    if (descriptor.kind == BrowseKind::SeriesSeasons) {
        const QString seriesId = descriptor.seriesId.isEmpty() ? descriptor.id : descriptor.seriesId;
        for (MovieItem& item : items) {
            if (item.seriesId.isEmpty())
                item.seriesId = seriesId;
        }
    }

    const int effectiveStart = std::max(0, startIndex);
    const int effectiveLimit = std::clamp(limit, 1, maximumLimit);
    const int totalCount = response.value(QStringLiteral("TotalRecordCount")).toInt(0);
    qInfo() << "browse page: request complete" << pageName << "start=" << effectiveStart << "limit=" << effectiveLimit
            << "items=" << items.size() << "total=" << totalCount << "ms=" << requestTimer.elapsed();
    co_return PagedMovieItems { std::move(items), totalCount, effectiveStart, effectiveLimit };
}

QCoro::Task<QVariantMap> JellyfinApiFacade::fetchLibraryFilterOptions(QString libraryId, QString collectionType)
{
    QVariantMap options;
    if (libraryId.isEmpty())
        co_return options;

    const QString includeItemTypes = includeItemTypesForCollection(collectionType);

    QUrlQuery filterQuery
        = ItemsQuery().userId(m_session.userId).parentId(libraryId).includeItemTypes(includeItemTypes).toUrlQuery();

    const QJsonObject filters
        = (co_await requestJson(HttpMethod::Get, QStringLiteral("/Items/Filters"), filterQuery)).object();
    options.insert(QStringLiteral("genres"), metaStringListFromJson(filters, { QStringLiteral("Genres") }));
    options.insert(
        QStringLiteral("officialRatings"), metaStringListFromJson(filters, { QStringLiteral("OfficialRatings") }));
    options.insert(QStringLiteral("tags"), metaStringListFromJson(filters, { QStringLiteral("Tags") }));

    QVariantList years;
    const QJsonArray yearsArray = filters.value(QStringLiteral("Years")).toArray();
    years.reserve(yearsArray.size());
    for (const QJsonValue& value : yearsArray) {
        const int year = value.toInt();
        if (year > 0)
            years.push_back(year);
    }
    options.insert(QStringLiteral("years"), years);

    QUrlQuery studiosQuery = ItemsQuery()
                                 .userId(m_session.userId)
                                 .parentId(libraryId)
                                 .includeItemTypes(includeItemTypes)
                                 .sort(QStringLiteral("SortName"))
                                 .toUrlQuery();

    const QJsonArray studioItems = (co_await requestJson(HttpMethod::Get, QStringLiteral("/Studios"), studiosQuery))
                                       .object()
                                       .value(QStringLiteral("Items"))
                                       .toArray();
    QVariantList studios;
    studios.reserve(studioItems.size());
    for (const QJsonValue& value : studioItems) {
        const QJsonObject studio = value.toObject();
        const QString id = studio.value(QStringLiteral("Id")).toString();
        const QString name = studio.value(QStringLiteral("Name")).toString();
        if (!id.isEmpty() && !name.isEmpty())
            studios.push_back(QVariantMap { { QStringLiteral("id"), id }, { QStringLiteral("name"), name } });
    }
    options.insert(QStringLiteral("studios"), studios);

    co_return options;
}

QCoro::Task<MovieItem> JellyfinApiFacade::fetchItemDetails(QString itemId)
{
    if (itemId.isEmpty() || m_session.userId.isEmpty())
        co_return MovieItem {};

    QUrlQuery query = ItemsQuery().fields(detailItemFields()).images().toUrlQuery();

    const QJsonObject object = (co_await requestJson(HttpMethod::Get,
                                    QStringLiteral("/Users/%1/Items/%2").arg(m_session.userId, itemId), query))
                                   .object();
    co_return mediaItemFromJson(object);
}

QCoro::Task<std::vector<MovieItem>> JellyfinApiFacade::fetchItemsByIds(QStringList itemIds)
{
    itemIds.removeAll(QString());
    itemIds.removeDuplicates();
    if (itemIds.isEmpty() || m_session.userId.isEmpty())
        co_return std::vector<MovieItem> {};

    std::vector<MovieItem> result;
    result.reserve(static_cast<size_t>(itemIds.size()));

    // SyncPlay queues commonly contain an entire library. Putting every UUID
    // into one query string exceeds Jellyfin/reverse-proxy request-line limits
    // (the TV observed HTTP 414 with a 262-item queue).
    constexpr qsizetype kItemsPerRequest = 50;
    for (qsizetype offset = 0; offset < itemIds.size(); offset += kItemsPerRequest) {
        const QStringList batch = itemIds.sliced(offset, std::min(kItemsPerRequest, itemIds.size() - offset));
        QUrlQuery query = ItemsQuery()
                              .userId(m_session.userId)
                              .fields(detailItemFields())
                              .images()
                              .add(QStringLiteral("ids"), batch.join(QLatin1Char(',')))
                              .toUrlQuery();
        const QJsonArray items = itemsArrayFromDocument(
            co_await requestJson(HttpMethod::Get, QStringLiteral("/Users/%1/Items").arg(m_session.userId), query));
        for (const QJsonValue& value : items)
            result.push_back(mediaItemFromJson(value.toObject()));
    }
    co_return result;
}

QCoro::Task<std::vector<MovieItem>> JellyfinApiFacade::fetchSeasons(QString seriesId)
{
    const PagedMovieItems page = co_await fetchBrowsePage(BrowseDescriptor::seriesSeasons(seriesId), 0, 200);
    co_return page.items;
}

QCoro::Task<std::vector<MovieItem>> JellyfinApiFacade::fetchEpisodes(QString seriesId, QString seasonId)
{
    constexpr int pageSize = 200;
    const BrowseDescriptor descriptor = BrowseDescriptor::seasonEpisodes(seriesId, seasonId);
    std::vector<MovieItem> episodes;
    int startIndex = 0;
    while (true) {
        PagedMovieItems page = co_await fetchBrowsePage(descriptor, startIndex, pageSize);
        if (page.items.empty())
            break;
        startIndex += static_cast<int>(page.items.size());
        episodes.insert(
            episodes.end(), std::make_move_iterator(page.items.begin()), std::make_move_iterator(page.items.end()));
        if (startIndex >= page.totalRecordCount || static_cast<int>(page.items.size()) < pageSize)
            break;
    }
    co_return episodes;
}

QCoro::Task<std::vector<MovieItem>> JellyfinApiFacade::fetchResumeItems(int limit)
{
    QUrlQuery query = ItemsQuery()
                          .userId(m_session.userId)
                          .limit(limit, 60)
                          .recursive()
                          .fields(libraryItemFields())
                          .includeItemTypes(QStringLiteral("Movie,Episode"))
                          .images()
                          .mediaTypes(QStringLiteral("Video"))
                          .toUrlQuery();

    const QJsonArray items = itemsArrayFromDocument(
        co_await requestJson(HttpMethod::Get, QStringLiteral("/Users/%1/Items/Resume").arg(m_session.userId), query));

    std::vector<MovieItem> result;
    result.reserve(items.size());
    for (const auto& value : items) {
        auto item = mediaItemFromJson(value.toObject());
        if (isPlayableItem(item) && item.resumeTicks > 0)
            result.push_back(item);
    }
    co_return result;
}

QCoro::Task<std::vector<MovieItem>> JellyfinApiFacade::fetchNextUpEpisodes(int limit)
{
    QUrlQuery query = ItemsQuery()
                          .userId(m_session.userId)
                          .limit(limit, 60)
                          .fields(libraryItemFields())
                          .images()
                          .add(QStringLiteral("enableResumable"), QStringLiteral("false"))
                          .toUrlQuery();

    const QJsonArray items
        = itemsArrayFromDocument(co_await requestJson(HttpMethod::Get, QStringLiteral("/Shows/NextUp"), query));

    std::vector<MovieItem> result;
    result.reserve(items.size());
    for (const auto& value : items) {
        auto item = mediaItemFromJson(value.toObject());
        if (item.itemType == QStringLiteral("Episode") && isPlayableItem(item))
            result.push_back(item);
    }
    co_return result;
}

QCoro::Task<std::vector<MovieItem>> JellyfinApiFacade::fetchLatestItems(QString parentId, int limit)
{
    QUrlQuery query = ItemsQuery()
                          .limit(limit, 200)
                          .fields(libraryItemFields())
                          .images()
                          .add(QStringLiteral("groupItems"), QStringLiteral("false"))
                          .parentId(parentId)
                          .toUrlQuery();

    const QJsonDocument doc
        = co_await requestJson(HttpMethod::Get, QStringLiteral("/Users/%1/Items/Latest").arg(m_session.userId), query);

    const QJsonArray items = itemsArrayFromDocument(doc);

    std::vector<MovieItem> result;
    result.reserve(items.size());
    for (const auto& value : items)
        result.push_back(mediaItemFromJson(value.toObject()));
    co_return result;
}

QCoro::Task<std::vector<MovieItem>> JellyfinApiFacade::searchItems(QString searchTerm, int limit)
{
    searchTerm = searchTerm.trimmed();
    if (searchTerm.isEmpty())
        co_return std::vector<MovieItem> {};

    const QStringList searchableTypes {
        QStringLiteral("Movie"),
        QStringLiteral("Series"),
        QStringLiteral("Season"),
        QStringLiteral("Episode"),
        QStringLiteral("MusicVideo"),
        QStringLiteral("Video"),
        QStringLiteral("Audio"),
        QStringLiteral("MusicAlbum"),
        QStringLiteral("MusicArtist"),
        QStringLiteral("Book"),
        QStringLiteral("AudioBook"),
        QStringLiteral("Photo"),
        QStringLiteral("PhotoAlbum"),
        QStringLiteral("Playlist"),
        QStringLiteral("BoxSet"),
        QStringLiteral("Folder"),
        QStringLiteral("Person"),
        QStringLiteral("Trailer"),
    };
    const auto fetchTypes = [this, &searchTerm](const QString& types, int requestLimit,
                                const QStringList& allowedTypes) -> QCoro::Task<std::vector<MovieItem>> {
        const QUrlQuery query = ItemsQuery()
                                    .userId(m_session.userId)
                                    .recursive()
                                    .add(QStringLiteral("searchTerm"), searchTerm)
                                    .includeItemTypes(types)
                                    .fields(libraryItemFields())
                                    .sort(QStringLiteral("SortName"))
                                    .images()
                                    .limit(requestLimit, 200)
                                    .toUrlQuery();
        const QJsonArray items = (co_await requestJson(HttpMethod::Get, QStringLiteral("/Items"), query))
                                     .object()
                                     .value(QStringLiteral("Items"))
                                     .toArray();
        co_return mediaItemsFromJson(items, allowedTypes);
    };

    std::vector<MovieItem> result = co_await fetchTypes(searchableTypes.join(QLatin1Char(',')), limit, searchableTypes);

    // Large episode libraries can consume a mixed result limit before any
    // matching series containers arrive. A small dedicated series query keeps
    // show search reliable without knowing anything about the server's library
    // names or layout.
    const std::vector<MovieItem> series
        = co_await fetchTypes(QStringLiteral("Series"), std::min(limit, 40), { QStringLiteral("Series") });
    QSet<QString> seen;
    for (const MovieItem& item : result)
        seen.insert(item.id);
    for (const MovieItem& item : series) {
        if (!seen.contains(item.id)) {
            seen.insert(item.id);
            result.push_back(item);
        }
    }

    co_return result;
}

QCoro::Task<std::vector<MovieItem>> JellyfinApiFacade::fetchSearchSuggestions(int limit)
{
    QUrlQuery query = ItemsQuery()
                          .userId(m_session.userId)
                          .recursive()
                          .includeItemTypes(QStringLiteral("Movie,Series"))
                          .mediaTypes(QStringLiteral("Video"))
                          .fields(libraryItemFields())
                          .sort(QStringLiteral("IsFavoriteOrLiked,Random"), {})
                          .images()
                          .enableTotalRecordCount(false)
                          .limit(limit, 60)
                          .toUrlQuery();

    const QJsonArray items = (co_await requestJson(HttpMethod::Get, QStringLiteral("/Items"), query))
                                 .object()
                                 .value(QStringLiteral("Items"))
                                 .toArray();

    const std::vector<MovieItem> result
        = mediaItemsFromJson(items, { QStringLiteral("Movie"), QStringLiteral("Series") });
    co_return result;
}

QCoro::Task<std::vector<MovieItem>> JellyfinApiFacade::fetchSimilarItems(QString itemId, int limit)
{
    if (itemId.isEmpty())
        co_return std::vector<MovieItem> {};

    QUrlQuery query
        = ItemsQuery().userId(m_session.userId).limit(limit, 60).fields(libraryItemFields()).images().toUrlQuery();

    const QJsonArray items = itemsArrayFromDocument(
        co_await requestJson(HttpMethod::Get, QStringLiteral("/Items/%1/Similar").arg(itemId), query));

    const std::vector<MovieItem> result
        = mediaItemsFromJson(items, { QStringLiteral("Movie"), QStringLiteral("Series"), QStringLiteral("Episode") });
    co_return result;
}

QCoro::Task<PersonCredits> JellyfinApiFacade::fetchItemsByPerson(QString personId, int maximumItems)
{
    PersonCredits credits;
    if (personId.isEmpty())
        co_return credits;

    const int boundedMaximum = std::clamp(maximumItems, 1, 10000);
    int startIndex = 0;
    while (startIndex < boundedMaximum) {
        const int pageLimit = std::min(200, boundedMaximum - startIndex);
        PagedMovieItems page = co_await fetchBrowsePage(BrowseDescriptor::person(personId), startIndex, pageLimit);
        if (page.items.empty())
            break;
        startIndex += static_cast<int>(page.items.size());
        credits.items.insert(credits.items.end(), std::make_move_iterator(page.items.begin()),
            std::make_move_iterator(page.items.end()));
        if ((page.totalRecordCount > 0 && startIndex >= page.totalRecordCount)
            || static_cast<int>(page.items.size()) < pageLimit)
            break;
    }

    QSet<QString> seriesIds;
    for (const MovieItem& item : credits.items) {
        if (item.itemType == QStringLiteral("Episode") && !item.seriesId.isEmpty())
            seriesIds.insert(item.seriesId);
    }

    const QStringList ids = seriesIds.values();
    for (qsizetype offset = 0; offset < ids.size(); offset += 200) {
        const QStringList batch = ids.sliced(offset, std::min<qsizetype>(200, ids.size() - offset));
        QUrlQuery query = ItemsQuery()
                              .userId(m_session.userId)
                              .includeItemTypes(QStringLiteral("Series"))
                              .fields(libraryItemFields() + QStringLiteral(",RecursiveItemCount"))
                              .images()
                              .limit(batch.size(), 200)
                              .add(QStringLiteral("ids"), batch.join(QLatin1Char(',')))
                              .toUrlQuery();
        const QJsonArray items = (co_await requestJson(HttpMethod::Get, QStringLiteral("/Items"), query))
                                     .object()
                                     .value(QStringLiteral("Items"))
                                     .toArray();
        std::vector<MovieItem> series = mediaItemsFromJson(items, { QStringLiteral("Series") });
        credits.relatedSeries.insert(credits.relatedSeries.end(), std::make_move_iterator(series.begin()),
            std::make_move_iterator(series.end()));
    }

    co_return credits;
}

QCoro::Task<std::vector<MovieItem>> JellyfinApiFacade::fetchManagementTargets(QString itemType)
{
    if (m_session.userId.isEmpty())
        co_return std::vector<MovieItem> {};

    itemType = itemType.trimmed();
    if (itemType != QStringLiteral("Playlist") && itemType != QStringLiteral("BoxSet"))
        co_return std::vector<MovieItem> {};

    QUrlQuery query = ItemsQuery()
                          .userId(m_session.userId)
                          .recursive()
                          .includeItemTypes(itemType)
                          .fields(libraryItemFields())
                          .sort(QStringLiteral("SortName"))
                          .images()
                          .limit(200, 200)
                          .toUrlQuery();

    const QJsonArray items = (co_await requestJson(HttpMethod::Get, QStringLiteral("/Items"), query))
                                 .object()
                                 .value(QStringLiteral("Items"))
                                 .toArray();
    co_return mediaItemsFromJson(items, { itemType });
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

QCoro::Task<void> JellyfinApiFacade::setItemFavorite(QString itemId, bool favorite)
{
    if (itemId.isEmpty())
        co_return;

    const QString path = QStringLiteral("/Users/%1/FavoriteItems/%2").arg(m_session.userId, itemId);
    co_await requestNoContent(favorite ? HttpMethod::Post : HttpMethod::Delete, path, QJsonDocument());
}

QCoro::Task<void> JellyfinApiFacade::setItemPlayed(QString itemId, bool played)
{
    if (itemId.isEmpty())
        co_return;

    const QString path = QStringLiteral("/Users/%1/PlayedItems/%2").arg(m_session.userId, itemId);
    co_await requestNoContent(played ? HttpMethod::Post : HttpMethod::Delete, path, QJsonDocument());
}

QCoro::Task<void> JellyfinApiFacade::setItemPlaybackPosition(QString itemId, qint64 positionTicks)
{
    if (itemId.isEmpty())
        co_return;

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("userId"), m_session.userId);
    const QJsonObject body {
        { QStringLiteral("ItemId"), itemId },
        { QStringLiteral("PlaybackPositionTicks"), std::max<qint64>(0, positionTicks) },
        { QStringLiteral("Played"), false },
    };
    co_await requestJson(
        HttpMethod::Post, QStringLiteral("/UserItems/%1/UserData").arg(itemId), query, QJsonDocument(body));
}

QCoro::Task<std::vector<MediaSegment>> JellyfinApiFacade::fetchMediaSegments(QString itemId)
{
    Diagnostics::Task task(QStringLiteral("api_fetch_media_segments"), { { QStringLiteral("itemId"), itemId } });
    std::vector<MediaSegment> result;
    try {
        const QJsonDocument doc
            = co_await requestJson(HttpMethod::Get, QStringLiteral("/MediaSegments/%1").arg(itemId));
        const QJsonArray items = itemsArrayFromDocument(doc);
        result.reserve(items.size());
        for (const auto& value : items) {
            const QJsonObject object = value.toObject();
            MediaSegment segment;
            segment.id = object.value(QStringLiteral("Id")).toString();
            segment.type = object.value(QStringLiteral("Type")).toString();
            // Jellyfin returns ticks as a JSON number; toVariant().toLongLong() handles ints and doubles.
            segment.startTicks = object.value(QStringLiteral("StartTicks")).toVariant().toLongLong();
            segment.endTicks = object.value(QStringLiteral("EndTicks")).toVariant().toLongLong();
            if (segment.endTicks > segment.startTicks)
                result.push_back(segment);
        }
    } catch (const std::exception& e) {
        // Servers without the media-segments endpoint return 404. Treat any
        // failure as "no segments" — playback should keep working.
        qInfo() << "api: media segments unavailable for" << itemId << ":" << e.what();
    }
    co_return result;
}

QCoro::Task<TrickplayInfo> JellyfinApiFacade::fetchTrickplayInfo(QString itemId, QString mediaSourceId)
{
    if (itemId.isEmpty() || m_session.userId.isEmpty())
        co_return TrickplayInfo {};

    const QUrlQuery query = ItemsQuery().fields(QStringLiteral("Trickplay")).toUrlQuery();
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
    command = command.trimmed();
    if (sessionId.isEmpty() || command.isEmpty())
        throw std::runtime_error("Remote playstate command needs a target and command");

    QUrlQuery query;
    if (seekPositionTicks >= 0)
        query.addQueryItem(QStringLiteral("seekPositionTicks"), QString::number(seekPositionTicks));
    if (!m_session.userId.isEmpty())
        query.addQueryItem(QStringLiteral("controllingUserId"), m_session.userId);
    const QString encodedSession = QString::fromLatin1(QUrl::toPercentEncoding(sessionId));
    const QString encodedCommand = QString::fromLatin1(QUrl::toPercentEncoding(command));
    co_await requestBytes(HttpMethod::Post,
        QStringLiteral("/Sessions/%1/Playing/%2").arg(encodedSession, encodedCommand), query, QJsonDocument());
}

QCoro::Task<void> JellyfinApiFacade::sendRemoteGeneralCommand(QString sessionId, QString command, QJsonObject arguments)
{
    sessionId = sessionId.trimmed();
    command = command.trimmed();
    if (sessionId.isEmpty() || command.isEmpty())
        throw std::runtime_error("Remote general command needs a target and command");

    QJsonObject body { { QStringLiteral("Name"), command } };
    if (!arguments.isEmpty())
        body.insert(QStringLiteral("Arguments"), arguments);
    const QString encodedSession = QString::fromLatin1(QUrl::toPercentEncoding(sessionId));
    co_await requestNoContent(
        HttpMethod::Post, QStringLiteral("/Sessions/%1/Command").arg(encodedSession), QJsonDocument(body));
}

QCoro::Task<QJsonArray> JellyfinApiFacade::fetchSyncPlayGroups()
{
    Diagnostics::Task task(QStringLiteral("api_syncplay_list"));
    try {
        const QJsonDocument doc = co_await requestJson(HttpMethod::Get, QStringLiteral("/SyncPlay/List"));
        if (doc.isArray())
            co_return doc.array();
        co_return doc.object().value(QStringLiteral("Items")).toArray();
    } catch (const std::exception& e) {
        qInfo() << "api: syncplay list failed:" << e.what();
        co_return QJsonArray();
    }
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
    co_return (co_await requestJson(HttpMethod::Get, QStringLiteral("/GetUtcTime"))).object();
}

QCoro::Task<void> JellyfinApiFacade::syncPlayReportPing(qint64 pingMs)
{
    const QJsonObject body = {
        { QStringLiteral("Ping"), std::max<qint64>(0, pingMs) },
    };
    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/SyncPlay/Ping"), QJsonDocument(body));
}

QCoro::Task<void> JellyfinApiFacade::syncPlayReportBuffering(
    bool buffering, qint64 positionTicks, bool playing, QString playlistItemId, QDateTime serverTime)
{
    if (playlistItemId.isEmpty()) {
        playlistItemId = QStringLiteral("00000000-0000-0000-0000-000000000000");
    }
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
        // Removing rows is not the same as ending the session, and the server
        // will happily do both if asked.
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

QCoro::Task<PlaybackSession> JellyfinApiFacade::negotiatePlayback(MovieItem movie, bool forceTranscode)
{
    Diagnostics::Task task(QStringLiteral("api_negotiate_playback"),
        { { QStringLiteral("itemId"), movie.id }, { QStringLiteral("title"), movie.title } });
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("userId"), m_session.userId);
    query.addQueryItem(QStringLiteral("fields"), QStringLiteral("Trickplay"));

    const QJsonObject body = {
        { QStringLiteral("UserId"), m_session.userId },
        { QStringLiteral("MaxStreamingBitrate"), m_maxStreamingBitrate },
        { QStringLiteral("StartTimeTicks"), movie.resumeTicks },
        { QStringLiteral("AutoOpenLiveStream"), true },
        { QStringLiteral("EnableDirectPlay"), !forceTranscode },
        { QStringLiteral("EnableDirectStream"), !forceTranscode },
        { QStringLiteral("EnableTranscoding"), true },
        { QStringLiteral("AllowVideoStreamCopy"), !forceTranscode },
        { QStringLiteral("AllowAudioStreamCopy"), true },
        { QStringLiteral("DeviceProfile"), buildDeviceProfile() },
    };

    const QJsonObject playbackResponse
        = (co_await requestJson(
               HttpMethod::Post, QStringLiteral("/Items/%1/PlaybackInfo").arg(movie.id), query, QJsonDocument(body)))
              .object();

    PlaybackSession session = buildPlaybackSession(movie, playbackResponse);
    if (session.trickplay.width <= 0) {
        const QUrlQuery itemQuery = ItemsQuery().fields(QStringLiteral("Trickplay")).toUrlQuery();
        const QJsonObject item = (co_await requestJson(HttpMethod::Get,
                                      QStringLiteral("/Users/%1/Items/%2").arg(m_session.userId, movie.id), itemQuery))
                                     .object();
        session.trickplay = PlaybackNegotiation::selectTrickplay(
            item.value(QStringLiteral("Trickplay")).toObject(), session.mediaSourceId, 320);
    }
    co_return session;
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

QJsonArray nowPlayingQueueJson(const std::vector<PlaybackQueueItem>& queue)
{
    QJsonArray array;
    for (const PlaybackQueueItem& item : queue) {
        QJsonObject object { { QStringLiteral("Id"), item.itemId } };
        if (!item.playlistItemId.isEmpty())
            object.insert(QStringLiteral("PlaylistItemId"), item.playlistItemId);
        array.push_back(object);
    }
    return array;
}

QCoro::Task<void> JellyfinApiFacade::reportPlaybackStart(
    PlaybackSession session, double playbackRate, int volume, bool muted)
{
    QJsonObject body = {
        { QStringLiteral("CanSeek"), true },
        { QStringLiteral("ItemId"), session.itemId },
        { QStringLiteral("MediaSourceId"), session.mediaSourceId },
        { QStringLiteral("PlayMethod"), session.playMethod },
        { QStringLiteral("PlaySessionId"), session.playSessionId },
        { QStringLiteral("PositionTicks"), session.startTimeTicks },
        { QStringLiteral("AudioStreamIndex"), session.audioStreamIndex },
        { QStringLiteral("SubtitleStreamIndex"), session.subtitleStreamIndex },
        { QStringLiteral("PlaybackRate"), playbackRate },
        { QStringLiteral("VolumeLevel"), std::clamp(volume, 0, 100) },
        { QStringLiteral("IsMuted"), muted },
    };
    if (!session.nowPlayingQueue.empty())
        body.insert(QStringLiteral("NowPlayingQueue"), nowPlayingQueueJson(session.nowPlayingQueue));

    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/Sessions/Playing"), QJsonDocument(body));
}

QCoro::Task<void> JellyfinApiFacade::reportPlaybackProgress(
    PlaybackSession session, qint64 positionTicks, bool paused, double playbackRate, int volume, bool muted)
{
    QJsonObject body = {
        { QStringLiteral("CanSeek"), true },
        { QStringLiteral("ItemId"), session.itemId },
        { QStringLiteral("MediaSourceId"), session.mediaSourceId },
        { QStringLiteral("PlayMethod"), session.playMethod },
        { QStringLiteral("PlaySessionId"), session.playSessionId },
        { QStringLiteral("PositionTicks"), positionTicks },
        { QStringLiteral("IsPaused"), paused },
        { QStringLiteral("AudioStreamIndex"), session.audioStreamIndex },
        { QStringLiteral("SubtitleStreamIndex"), session.subtitleStreamIndex },
        { QStringLiteral("PlaybackRate"), playbackRate },
        { QStringLiteral("VolumeLevel"), std::clamp(volume, 0, 100) },
        { QStringLiteral("IsMuted"), muted },
    };
    if (!session.nowPlayingQueue.empty())
        body.insert(QStringLiteral("NowPlayingQueue"), nowPlayingQueueJson(session.nowPlayingQueue));

    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/Sessions/Playing/Progress"), QJsonDocument(body));
}

QCoro::Task<void> JellyfinApiFacade::reportPlaybackStopped(
    PlaybackSession session, qint64 positionTicks, bool failed, double playbackRate)
{
    const QJsonObject body = {
        { QStringLiteral("ItemId"), session.itemId },
        { QStringLiteral("MediaSourceId"), session.mediaSourceId },
        { QStringLiteral("PlaySessionId"), session.playSessionId },
        { QStringLiteral("PositionTicks"), positionTicks },
        { QStringLiteral("Failed"), failed },
        { QStringLiteral("PlaybackRate"), playbackRate },
    };

    co_await requestNoContent(HttpMethod::Post, QStringLiteral("/Sessions/Playing/Stopped"), QJsonDocument(body));
}

QNetworkRequest JellyfinApiFacade::createRequest(const QString& path, const QUrlQuery& query) const
{
    QNetworkRequest request
        = query.isEmpty() ? m_requestFactory.createRequest(path) : m_requestFactory.createRequest(path, query);
    if (!HttpRequestPolicy::allowsCredentialTransport(request.url()))
        throw std::runtime_error("Credentials require HTTPS or a numeric private/loopback HTTP address");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Authorization", authorizationHeader().toUtf8());
    if (path == QStringLiteral("/Playback/BitrateTest") || path == QStringLiteral("/System/Endpoint")) {
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
        request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
        if (path == QStringLiteral("/Playback/BitrateTest"))
            request.setRawHeader("Accept", "application/octet-stream");
    }
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

QJsonObject JellyfinApiFacade::buildDeviceProfile() const
{
    return PlaybackNegotiation::buildDeviceProfile(
        m_maxStreamingBitrate, maxStreamingHeight(), m_videoCodecs, m_restrictVideoCodecs);
}

PlaybackSession JellyfinApiFacade::buildPlaybackSession(
    const MovieItem& movie, const QJsonObject& playbackResponse) const
{
    const QJsonArray mediaSources = playbackResponse.value(QStringLiteral("MediaSources")).toArray();
    if (mediaSources.isEmpty())
        throw std::runtime_error("No media sources returned by Jellyfin");

    const PlaybackSelection selection = PlaybackNegotiation::selectSource(mediaSources, m_preferRemux);
    const QJsonObject selectedSource = selection.source;

    const QString mediaSourceId = requireString(selectedSource, QStringLiteral("Id"));
    const QString container = cleanContainerName(selectedSource.value(QStringLiteral("Container")).toString());

    TrickplayInfo trickplay = PlaybackNegotiation::selectTrickplay(
        playbackResponse.value(QStringLiteral("Trickplay")).toObject(), mediaSourceId, 320);
    if (trickplay.width <= 0) {
        trickplay = PlaybackNegotiation::selectTrickplay(
            selectedSource.value(QStringLiteral("Trickplay")).toObject(), mediaSourceId, 320);
    }

    return {
        movie.id,
        movie.title,
        movie.itemType,
        PlaybackNegotiation::buildUrl(m_serverUrl, movie.id, selection),
        mediaSourceId,
        playbackResponse.value(QStringLiteral("PlaySessionId")).toString(),
        selection.playMethod,
        container,
        movie.resumeTicks,
        movie.runtimeTicks,
        mediaStreamsFromApiJson(selectedSource.value(QStringLiteral("MediaStreams")).toArray()),
        {},
        trickplay,
        {},
    };
}

} // namespace JellyfinNative
