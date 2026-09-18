#pragma once

#include "../provider/Provider.h"

#include <QCoroTask>
#include <QObject>
#include <QString>

class QNetworkAccessManager;

namespace JellyfinNative {

class BrowseSessionController;
class DatabaseManager;
class DiscoveredServerModel;
class DiscoveryController;
class JellyfinApiFacade;
class JellyfinSettingsBridge;
class LibraryManagementController;
class PlayQueueController;
class PlayerController;
class QuickConnectController;
class RemoteControlController;
class SessionController;
class SettingsController;
class SyncPlayController;
class TlsTrustController;

struct JellyfinProviderContext {
    QNetworkAccessManager *network = nullptr;
    TlsTrustController *tlsTrust = nullptr;
    DatabaseManager *database = nullptr;
    QString deviceName;
    QString appVersion;
};

// Everything Jellyfin-specific, composed in one place: the API facade, LAN
// discovery and its cached server list, the session and its sign-in helpers,
// SyncPlay, remote control and library management. The app hands it the
// core objects those parts sit on through attach(); the provider then keeps
// the session-driven lifecycle of its own parts (discovery scans while
// nobody is signed in, the SyncPlay socket and remote-control registration
// follow the session) so the app never has to.
class JellyfinProvider final : public Provider {
    Q_OBJECT

public:
    explicit JellyfinProvider(const JellyfinProviderContext& context, QObject *parent = nullptr);
    ~JellyfinProvider() override;

    QString id() const override;
    QString displayName() const override;
    Capabilities capabilities() const override;
    PlaybackSource *playback() override;
    void registerQmlSingletons() override;

    // The core objects built after the provider exists. The parts created
    // here are parented to `owner`, which must not outlive `player`: they
    // hold it by pointer.
    struct CoreServices {
        QObject *owner = nullptr;
        PlayerController *player = nullptr;
        PlayQueueController *playQueue = nullptr;
        SettingsController *settings = nullptr;
        BrowseSessionController *browse = nullptr;
    };
    void attach(const CoreServices& core);

    // Shows the servers seen last time and scans for more while nobody is
    // signed in. The app calls it once its stored session is known; the
    // provider calls it itself after a sign-out.
    void resumeServerDiscovery();
    void shutdown();

    JellyfinApiFacade *api() const
    {
        return m_api;
    }
    DiscoveryController *discovery() const
    {
        return m_discovery;
    }
    DiscoveredServerModel *discoveredServers() const
    {
        return m_discoveredServers;
    }
    SessionController *session() const
    {
        return m_session;
    }
    QuickConnectController *quickConnect() const
    {
        return m_quickConnect;
    }
    RemoteControlController *remoteControl() const
    {
        return m_remoteControl;
    }
    // Null until attach().
    SyncPlayController *syncPlay() const
    {
        return m_syncPlay;
    }
    LibraryManagementController *management() const
    {
        return m_management;
    }
    JellyfinSettingsBridge *settingsBridge() const
    {
        return m_settingsBridge;
    }

private:
    void cacheDiscoveredServers();
    QCoro::Task<void> restoreDiscoveredServersAsync();
    void leaveSessionServices();

    DatabaseManager *m_database = nullptr;
    TlsTrustController *m_tlsTrust = nullptr;
    JellyfinApiFacade *m_api = nullptr;
    DiscoveryController *m_discovery = nullptr;
    DiscoveredServerModel *m_discoveredServers = nullptr;
    SessionController *m_session = nullptr;
    QuickConnectController *m_quickConnect = nullptr;
    RemoteControlController *m_remoteControl = nullptr;
    SyncPlayController *m_syncPlay = nullptr;
    LibraryManagementController *m_management = nullptr;
    JellyfinSettingsBridge *m_settingsBridge = nullptr;
};

} // namespace JellyfinNative
