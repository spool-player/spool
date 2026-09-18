#pragma once

#include "../provider/Provider.h"

#include <QCoroTask>
#include <QObject>
#include <QString>

class QNetworkAccessManager;

namespace JellyfinNative {

class ArtworkService;
class DatabaseManager;
class DiscoveredServerModel;
class DiscoveryController;
class JellyfinApiFacade;
class JellyfinSettingsBridge;
class LibraryManagementController;
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
// follow the session, artwork carries the session's token) so the app never
// has to. The app sees it only as a Provider.
class JellyfinProvider final : public Provider {
    Q_OBJECT

public:
    explicit JellyfinProvider(const JellyfinProviderContext& context, QObject *parent = nullptr);
    ~JellyfinProvider() override;

    QString id() const override;
    QString displayName() const override;
    Capabilities capabilities() const override;
    PlaybackSource *playback() override;
    Catalog *catalog() override;
    ArtworkSource *artwork() override;
    SearchSource *search() override;
    UserItemStateSink *itemState() override;
    StreamQualityControl *streamQuality() override;
    GroupPlayback *groupPlayback() override;
    RemotePlayback *remotePlayback() override;

    void attach(const CoreServices& core) override;
    void registerQmlSingletons() override;
    void shutdown() override;

    bool ready() const override;
    bool hasDefaultProfile() const override
    {
        return m_hasDefaultProfile;
    }
    QStringList startupStorageKeys() const override;
    bool restoreFromStorage(QVariantMap values, std::vector<AccountProfile> profiles) override;
    void setDeviceId(const QString& deviceId) override;
    void setLocale(const QString& bcp47) override;
    bool handleUnauthorized(const std::exception_ptr& error) override;

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
    // Shows the servers seen last time and scans for more while nobody is
    // signed in. Runs after the stored session is known and after a sign-out.
    void resumeServerDiscovery();
    void cacheDiscoveredServers();
    QCoro::Task<void> restoreDiscoveredServersAsync();
    void leaveSessionServices();
    void setHasDefaultProfile(bool hasDefaultProfile);

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
    SettingsController *m_settings = nullptr;
    ArtworkService *m_artwork = nullptr;
    bool m_hasDefaultProfile = false;
};

} // namespace JellyfinNative
