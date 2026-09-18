#include "JellyfinProvider.h"

#include "../app/LibraryManagementController.h"
#include "../app/QuickConnectController.h"
#include "../app/RemoteControlController.h"
#include "../app/SessionController.h"
#include "../app/SyncPlayController.h"
#include "../cache/DatabaseManager.h"
#include "../common/AsyncTask.h"
#include "../common/MetaJson.h"
#include "../discovery/DiscoveryController.h"
#include "../models/DiscoveredServerModel.h"
#include "../platform/PlatformPlaybackRuntime.h"
#include "JellyfinApiFacade.h"
#include "JellyfinSettingsBridge.h"
#if defined(JELLYFIN_NATIVE_WEBOS)
#include "../platform/webos/WebOSDeviceName.h"
#endif

#include <QDebug>
#include <QJsonArray>
#include <QPointer>
#include <QQmlEngine>

namespace JellyfinNative {

JellyfinProvider::JellyfinProvider(const JellyfinProviderContext& context, QObject *parent)
    : Provider(parent)
    , m_database(context.database)
    , m_tlsTrust(context.tlsTrust)
{
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
        [api = QPointer<JellyfinApiFacade>(m_api)](const QStringList& videoCodecs, bool restrictVideoCodecs) {
            if (api)
                api->setVideoCodecCapabilities(videoCodecs, restrictVideoCodecs);
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

    // The session drives the parts that only make sense while signed in.
    connect(m_session, &SessionController::authenticatedChanged, this, [this](const AuthSession&) {
        if (m_syncPlay)
            m_syncPlay->connectSocket();
        m_remoteControl->start();
        m_discovery->stop();
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
        | RemoteControl | QuickConnect | PeerRelay;
}

PlaybackSource *JellyfinProvider::playback()
{
    return m_api;
}

void JellyfinProvider::attach(const CoreServices& core)
{
    Q_ASSERT(!m_syncPlay);
    m_syncPlay = new SyncPlayController(m_api, core.player, core.playQueue, m_tlsTrust, core.owner);
    m_management = new LibraryManagementController(m_api, core.browse, core.owner);
    m_settingsBridge = new JellyfinSettingsBridge(core.settings, m_api, core.owner);
    connect(m_syncPlay, &SyncPlayController::sessionsUpdated, m_remoteControl, &RemoteControlController::applySessions);
    connect(m_syncPlay, &QObject::destroyed, this, [this]() { m_syncPlay = nullptr; });
    connect(m_management, &QObject::destroyed, this, [this]() { m_management = nullptr; });
    connect(m_settingsBridge, &QObject::destroyed, this, [this]() { m_settingsBridge = nullptr; });
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

void JellyfinProvider::shutdown()
{
    m_quickConnect->cancel();
    m_api->cancelRequests();
    m_discovery->stop();
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

} // namespace JellyfinNative
