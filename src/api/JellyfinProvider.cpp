#include "JellyfinProvider.h"

#include "../app/ArtworkService.h"
#include "../app/LibraryManagementController.h"
#include "../app/QuickConnectController.h"
#include "../app/RemoteControlController.h"
#include "../app/SessionController.h"
#include "../app/SettingsController.h"
#include "../app/SyncPlayController.h"
#include "../cache/DatabaseManager.h"
#include "../common/AsyncTask.h"
#include "../common/MetaJson.h"
#include "../discovery/DiscoveryController.h"
#include "../models/DiscoveredServerModel.h"
#include "../platform/PlatformPlaybackRuntime.h"
#include "../player/PlayerController.h"
#include "../provider/ProviderRegistry.h"
#include "JellyfinApiFacade.h"
#include "JellyfinJsAdapter.h"
#include "JellyfinSettingsBridge.h"
#if defined(JELLYFIN_NATIVE_WEBOS)
#include "../platform/webos/WebOSDeviceName.h"
#endif

#include <QDebug>
#include <QJsonArray>
#include <QPointer>
#include <QQmlEngine>
#include <QTimer>

namespace JellyfinNative {

JellyfinProvider::JellyfinProvider(const JellyfinProviderContext& context, QObject *parent)
    : Provider(parent)
    , m_registry(context.registry)
    , m_deviceName(context.deviceName)
    , m_appVersion(context.appVersion)
    , m_database(context.database)
    , m_tlsTrust(context.tlsTrust)
{
    if (!m_registry)
        throw std::runtime_error("javascript_provider_unavailable");
    m_jsAdapter = std::make_unique<JellyfinJsAdapter>(m_registry, this);

    m_api = new JellyfinApiFacade(context.network, context.tlsTrust, this);
    m_api->setDeviceIdentity({}, context.deviceName, context.appVersion);
#if defined(JELLYFIN_NATIVE_WEBOS)
    // webOS reports the name the owner gave the set only on request, and
    // again whenever they change it. Marshal it onto the API's thread, since
    // the luna callback does not belong to us.
    requestWebOSDeviceName([api = QPointer<JellyfinApiFacade>(m_api)](const QString& name) {
        if (!api)
            return;
        QMetaObject::invokeMethod(
            api,
            [api, name]() {
                if (api)
                    api->setDeviceName(name);
            },
            Qt::QueuedConnection);
    });
#endif
    configurePlatformPlaybackCapabilities(
        [this](const QStringList& videoCodecs, bool restrictVideoCodecs) {
            if (m_jsAdapter)
                m_jsAdapter->setVideoCodecCapabilities(videoCodecs, restrictVideoCodecs);
        },
        *this);

    m_discovery = new DiscoveryController(context.tlsTrust, this);
    m_discoveredServers = new DiscoveredServerModel(this);
    m_session = new SessionController(context.database, m_api, this);
    m_session->setDiscoveredServers(m_discoveredServers);
    m_quickConnect = new QuickConnectController(m_api, this);
    m_remoteControl = new RemoteControlController(m_api, this);

    connect(m_discovery, &DiscoveryController::serverDiscovered, this, [this](const DiscoveredServer& server) {
        m_discoveredServers->upsertServer(server);
        cacheDiscoveredServers();
    });
    connect(m_session, &SessionController::serverRemembered, this, &JellyfinProvider::cacheDiscoveredServers);
    connect(m_api, &JellyfinApiFacade::authenticationExpired, m_session, &SessionController::expireSession);
    connect(m_quickConnect, &QuickConnectController::authenticated, this,
        [this](const AuthSession& session) { m_session->acceptSession(session); });

    // What the app shows for the sign-in parts: one busy flag, one error
    // line, toasts for the rest.
    connect(m_session, &SessionController::busyChanged, this, &Provider::busyChanged);
    connect(m_session, &SessionController::errorOccurred, this, &Provider::errorOccurred);
    connect(m_quickConnect, &QuickConnectController::busyChanged, this, &Provider::busyChanged);
    connect(m_quickConnect, &QuickConnectController::errorOccurred, this, &Provider::errorOccurred);
    connect(m_remoteControl, &RemoteControlController::errorText, this, &Provider::toastRequested);
    connect(m_remoteControl, &RemoteControlController::feedbackText, this, &Provider::toastRequested);
    connect(m_session, &SessionController::accountProfilesChanged, this,
        [this]() { setHasDefaultProfile(!m_session->accountProfiles().isEmpty()); });
    connect(m_session, &SessionController::profileActivationStarted, this, &Provider::activationStarted);
    connect(m_session, &SessionController::switchUserRequested, this, &Provider::switchUserRequested);
    connect(m_session, &SessionController::logoutStarted, this, &Provider::signOutStarted);

    // The session drives the parts that only make sense while signed in.
    connect(m_session, &SessionController::authenticatedChanged, this, [this](const AuthSession& auth) {
        Async::runScoped(
            this, configureJsSourceAsync(auth), []() {},
            [this](const std::exception_ptr& error) { emit errorOccurred(exceptionMessage(error)); });

        if (m_artwork)
            m_artwork->setAuthorizationHeader(m_api->authorizationHeader());
        if (m_syncPlay)
            m_syncPlay->connectSocket();
        m_remoteControl->start();
        m_discovery->stop();
        setHasDefaultProfile(true);
        emit sessionStarted();
    });
    connect(m_session, &SessionController::profileActivationStarted, this, [this]() {
        m_quickConnect->cancel();
        leaveSessionServices();
    });
    connect(m_session, &SessionController::switchUserRequested, this, [this]() {
        m_quickConnect->cancel();
        m_discovery->start();
    });
    connect(m_session, &SessionController::logoutStarted, this, [this]() {
        m_quickConnect->cancel();
        leaveSessionServices();
    });
    connect(m_session, &SessionController::loggedOut, this, [this]() {
        leaveSessionServices();
        if (m_jsAdapter)
            m_jsAdapter->clear();
        m_jsSourceId.clear();
        if (m_artwork)
            m_artwork->setAuthorizationHeader({});
        if (m_management)
            m_management->clear();
        emit sessionEnded();
        resumeServerDiscovery();
    });
}

JellyfinProvider::~JellyfinProvider() = default;

QString JellyfinProvider::id() const
{
    return QStringLiteral("jellyfin");
}

QString JellyfinProvider::displayName() const
{
    return QStringLiteral("Jellyfin");
}

Provider::Capabilities JellyfinProvider::capabilities() const
{
    return Auth | Discovery | Search | UserItemState | PlaybackReporting | Segments | LibraryManagement | SyncPlay
        | RemoteControl | QuickConnect | PeerRelay | StreamQuality;
}

PlaybackSource *JellyfinProvider::playback()
{
    return m_jsAdapter->playback();
}

Catalog *JellyfinProvider::catalog()
{
    return m_jsAdapter->catalog();
}

ArtworkSource *JellyfinProvider::artwork()
{
    return m_jsAdapter->artwork();
}

SearchSource *JellyfinProvider::search()
{
    return m_jsAdapter->search();
}

UserItemStateSink *JellyfinProvider::itemState()
{
    return m_jsAdapter->itemState();
}

StreamQualityControl *JellyfinProvider::streamQuality()
{
    return m_jsAdapter.get();
}

GroupPlayback *JellyfinProvider::groupPlayback()
{
    return m_syncPlay;
}

RemotePlayback *JellyfinProvider::remotePlayback()
{
    return m_remoteControl;
}

void JellyfinProvider::attach(const CoreServices& core)
{
    Q_ASSERT(!m_syncPlay);
    m_settings = core.settings;
    m_artwork = core.artwork;
    m_syncPlay = new SyncPlayController(m_api, core.player, core.playQueue, m_tlsTrust, core.owner);
    m_management = new LibraryManagementController(m_api, core.browse, core.owner);
    m_settingsBridge = new JellyfinSettingsBridge(core.settings, m_api, core.owner);
    connect(m_syncPlay, &SyncPlayController::sessionsUpdated, m_remoteControl, &RemoteControlController::applySessions);
    connect(m_syncPlay, &QObject::destroyed, this, [this]() { m_syncPlay = nullptr; });
    connect(m_management, &QObject::destroyed, this, [this]() { m_management = nullptr; });
    connect(m_settingsBridge, &QObject::destroyed, this, [this]() { m_settingsBridge = nullptr; });

    connect(m_syncPlay, &SyncPlayController::errorText, this, &Provider::toastRequested);
    connect(m_management, &LibraryManagementController::errorOccurred, this, &Provider::toastRequested);
    connect(m_management, &LibraryManagementController::operationSucceeded, this, &Provider::toastRequested);
    connect(m_management, &LibraryManagementController::refreshRequested, this, &Provider::contentChanged);
    connect(m_settingsBridge, &JellyfinSettingsBridge::errorOccurred, this, &Provider::toastRequested);

    // Commands other clients send arrive over the SyncPlay socket; they
    // reach the app only while this device is set to accept them.
    const auto forward = [this](void (RemotePlayback::*signal)(const QJsonObject&)) {
        return [this, signal](const QJsonObject& data) {
            if (m_api->remoteControlTargetEnabled())
                emit(m_remoteControl->*signal)(data);
        };
    };
    connect(m_syncPlay, &SyncPlayController::remotePlayCommand, m_remoteControl,
        forward(&RemotePlayback::playCommandReceived));
    connect(m_syncPlay, &SyncPlayController::remotePlaystateCommand, m_remoteControl,
        forward(&RemotePlayback::playstateCommandReceived));
    connect(m_syncPlay, &SyncPlayController::remoteGeneralCommand, m_remoteControl,
        forward(&RemotePlayback::generalCommandReceived));
    if (SpoolLink *link = m_remoteControl->link())
        connect(link, &SpoolLink::messageReceived, m_remoteControl, &RemotePlayback::peerMessageReceived);
}

void JellyfinProvider::registerQmlSingletons()
{
    Q_ASSERT(m_syncPlay && m_management);
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "Session", m_session);
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "QuickConnect", m_quickConnect);
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "SyncPlay", m_syncPlay);
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "RemoteControl", m_remoteControl);
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "Management", m_management);
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "Discovery", m_discovery);
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "DiscoveredServers", m_discoveredServers);
}

void JellyfinProvider::shutdown()
{
    m_quickConnect->cancel();
    m_api->cancelRequests();
    m_discovery->stop();
}

bool JellyfinProvider::ready() const
{
    return m_session->authenticated() && m_jsAdapter && m_jsAdapter->signedIn();
}

QStringList JellyfinProvider::startupStorageKeys() const
{
    return SessionController::localStorageKeys();
}

bool JellyfinProvider::restoreFromStorage(QVariantMap values, std::vector<AccountProfile> profiles)
{
    const bool hasDefaultProfile = m_session->initializeFromStorage(std::move(values), std::move(profiles));
    setHasDefaultProfile(hasDefaultProfile);
    if (!m_session->authenticated())
        resumeServerDiscovery();
    return hasDefaultProfile;
}

void JellyfinProvider::setDeviceId(const QString& deviceId)
{
    m_deviceId = deviceId;
    m_api->setDeviceId(deviceId);
    if (m_jsAdapter)
        m_jsAdapter->setDeviceId(deviceId);
}

void JellyfinProvider::setLocale(const QString& bcp47)
{
    m_api->setAcceptLanguage(bcp47);
}

bool JellyfinProvider::handleUnauthorized(const std::exception_ptr& error)
{
    return m_session->handleUnauthorized(error);
}

QCoro::Task<void> JellyfinProvider::configureJsSourceAsync(const AuthSession& session)
{
    if (!m_registry)
        co_return;
    const QString serverUrl = m_session->serverUrl();
    if (serverUrl.isEmpty() || session.userId.isEmpty() || session.accessToken.isEmpty())
        co_return;

    QVariantMap config { { QStringLiteral("server"), serverUrl }, { QStringLiteral("userId"), session.userId },
        { QStringLiteral("token"), session.accessToken },
        { QStringLiteral("deviceId"), m_deviceId.isEmpty() ? QStringLiteral("spool") : m_deviceId },
        { QStringLiteral("deviceName"), m_deviceName.isEmpty() ? QStringLiteral("Spool") : m_deviceName },
        { QStringLiteral("clientVersion"), m_appVersion.isEmpty() ? QStringLiteral("0.1.0") : m_appVersion } };
    QList<QUrl> origins { QUrl(serverUrl) };
    const QString sourceId = co_await m_registry->configureSource(
        QStringLiteral("spool.jellyfin"), session.userId, QStringLiteral("library"), serverUrl, config, origins);

    m_jsSourceId = sourceId;
    m_jsAdapter->configure(sourceId, serverUrl, session.accessToken);
    if (m_artwork)
        m_artwork->setAuthorizationHeader(m_api->authorizationHeader());
    m_discovery->stop();
    setHasDefaultProfile(true);
    emit sessionStarted();
}

void JellyfinProvider::resumeServerDiscovery()
{
    Async::runScoped(
        this, restoreDiscoveredServersAsync(),
        [this]() {
            if (!m_session->authenticated())
                m_discovery->start();
        },
        [this](const std::exception_ptr& error) {
            qWarning() << "discovery: cached server load failed" << exceptionMessage(error);
            if (!m_session->authenticated())
                m_discovery->start();
        },
        "discovery cache");
}

void JellyfinProvider::cacheDiscoveredServers()
{
    QJsonArray cache;
    for (const auto& entry : m_discoveredServers->servers())
        cache.push_back(metaToJson(entry));
    m_database->saveDiscoveredServers(cache);
}

QCoro::Task<void> JellyfinProvider::restoreDiscoveredServersAsync()
{
    const auto servers = co_await m_database->loadDiscoveredServersAsync();
    std::vector<DiscoveredServer> parsed;
    parsed.reserve(servers.size());
    for (const auto& value : servers)
        parsed.push_back(metaFromJson<DiscoveredServer>(value.toObject()));
    m_discoveredServers->setServers(parsed, false);
}

void JellyfinProvider::leaveSessionServices()
{
    if (m_syncPlay)
        m_syncPlay->disconnectSocket();
    m_remoteControl->stop();
}

void JellyfinProvider::setHasDefaultProfile(bool hasDefaultProfile)
{
    if (m_hasDefaultProfile == hasDefaultProfile)
        return;
    m_hasDefaultProfile = hasDefaultProfile;
    emit hasDefaultProfileChanged();
}

} // namespace JellyfinNative
