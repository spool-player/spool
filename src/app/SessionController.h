#pragma once

#include "../api/JellyfinSession.h"
#include "../media/MediaTypes.h"
#include "AccountProfile.h"

#include <QCoroTask>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <exception>
#include <vector>

namespace JellyfinNative {

class DatabaseManager;
class DiscoveredServerModel;
class JellyfinApiFacade;

class SessionController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString serverUrl READ serverUrl WRITE setServerUrl NOTIFY serverUrlChanged)
    Q_PROPERTY(QString serverName READ serverName WRITE setServerName NOTIFY serverNameChanged)
    Q_PROPERTY(QString username READ username WRITE setUsername NOTIFY usernameChanged)
    Q_PROPERTY(QString password READ password WRITE setPassword NOTIFY passwordChanged)
    Q_PROPERTY(bool authenticated READ authenticated NOTIFY authenticatedStateChanged)
    Q_PROPERTY(QString activeProfileId READ activeProfileId NOTIFY activeProfileChanged)
    Q_PROPERTY(QString activeProfileLabel READ activeProfileLabel NOTIFY activeProfileChanged)
    Q_PROPERTY(QVariantList accountProfiles READ accountProfiles NOTIFY accountProfilesChanged)
    Q_PROPERTY(bool profileSignInRequired READ profileSignInRequired NOTIFY profileSignInRequiredChanged)

public:
    SessionController(DatabaseManager *database, JellyfinApiFacade *api, QObject *parent = nullptr);

    QString serverUrl() const;
    QString serverName() const;
    QString username() const;
    QString password() const;
    bool authenticated() const;
    QString activeProfileId() const;
    QString activeProfileLabel() const;
    QVariantList accountProfiles() const;
    bool profileSignInRequired() const;

    QCoro::Task<bool> initializeAsync();
    static QStringList localStorageKeys();
    bool initializeFromStorage(QVariantMap values, std::vector<AccountProfile> profiles);
    // The list the server step offers; the provider owns it and caches it.
    void setDiscoveredServers(DiscoveredServerModel *servers);
    Q_INVOKABLE void chooseDiscoveredServer(int index);
    Q_INVOKABLE void rememberServer(const QString& name, const QString& address);
    // Back to the profile picker while the session is kept, so a cancel
    // returns to it; the provider scans for servers meanwhile.
    Q_INVOKABLE void switchUser();
    Q_INVOKABLE void setServerUrl(const QString& serverUrl);
    Q_INVOKABLE void setServerName(const QString& serverName);
    Q_INVOKABLE void setUsername(const QString& username);
    Q_INVOKABLE void setPassword(const QString& password);
    Q_INVOKABLE void login();
    Q_INVOKABLE void activateProfile(const QString& profileId);
    Q_INVOKABLE void prepareProfileSignIn(const QString& profileId);
    Q_INVOKABLE void updateProfileServer(const QString& profileId, const QString& name, const QString& url);
    Q_INVOKABLE void removeProfile(const QString& profileId);
    Q_INVOKABLE void clearProfiles();
    Q_INVOKABLE void deactivate();
    void acceptSession(const AuthSession& session);
    Q_INVOKABLE void logout();
    void expireSession(const QString& message);
    bool handleUnauthorized(const std::exception_ptr& error);

signals:
    void serverUrlChanged();
    void serverNameChanged();
    void usernameChanged();
    void passwordChanged();
    void busyChanged(bool busy, const QString& text);
    void errorOccurred(const QString& message);
    void authenticatedChanged(const JellyfinNative::AuthSession& session);
    void authenticatedStateChanged();
    void activeProfileChanged();
    void accountProfilesChanged();
    void profileSignInRequiredChanged();
    void loggedOut();
    // A remembered or chosen server was added to the list.
    void serverRemembered();
    // Lifecycle points the provider hangs its own parts on: sign-in code
    // polling stops, the SyncPlay socket and remote-control registration are
    // dropped before the session changes hands.
    void profileActivationStarted();
    void switchUserRequested();
    void logoutStarted();

private:
    QCoro::Task<void> activateProfileAsync(const QString& profileId);
    bool applyStoredProfile(AccountProfile profile, bool persistUsage);
    void activateSession(const AuthSession& session, bool persist);
    void upsertActiveProfile(const AuthSession& session);
    void setProfileSignInFields(const AccountProfile& profile);
    void clearActiveSession();
    void clearPassword();
    void sortProfiles();

    DatabaseManager *m_database = nullptr;
    JellyfinApiFacade *m_api = nullptr;
    DiscoveredServerModel *m_discoveredServers = nullptr;
    QString m_serverUrl;
    QString m_serverName;
    QString m_username;
    QString m_password;
    QString m_activeProfileId;
    std::vector<AccountProfile> m_profiles;
    bool m_profileSignInRequired = false;
};

} // namespace JellyfinNative
