#include "SessionController.h"

#include "../api/JellyfinApiFacade.h"
#include "../cache/DatabaseManager.h"
#include "../common/AsyncTask.h"
#include "../models/DiscoveredServerModel.h"

#include <QDateTime>
#include <QDebug>
#include <QUrl>

#include <algorithm>

namespace JellyfinNative {

SessionController::SessionController(DatabaseManager *database, JellyfinApiFacade *api, QObject *parent)
    : QObject(parent)
    , m_database(database)
    , m_api(api)
{
    connect(m_api, &JellyfinApiFacade::deviceProfileChanged, this, [this]() {
        if (!authenticated())
            return;
        Async::runScoped(
            this, m_api->postCapabilities(), []() {},
            [](const std::exception_ptr& error) {
                qWarning() << "session: updated capability report failed" << exceptionMessage(error);
            });
    });
}

QString SessionController::serverUrl() const
{
    return m_serverUrl;
}

QString SessionController::serverName() const
{
    return m_serverName;
}

QString SessionController::username() const
{
    return m_username;
}

QString SessionController::password() const
{
    return m_password;
}

bool SessionController::authenticated() const
{
    return !m_api->session().accessToken.isEmpty();
}

QString SessionController::activeProfileId() const
{
    return m_activeProfileId;
}

QString SessionController::activeProfileLabel() const
{
    const auto it = std::find_if(m_profiles.cbegin(), m_profiles.cend(),
        [this](const AccountProfile& profile) { return profile.profileId == m_activeProfileId; });
    return it == m_profiles.cend() ? QString() : it->displayLabel();
}

bool SessionController::profileSignInRequired() const
{
    return m_profileSignInRequired;
}

QVariantList SessionController::accountProfiles() const
{
    QVariantList result;
    result.reserve(static_cast<qsizetype>(m_profiles.size()));
    for (const AccountProfile& profile : m_profiles) {
        const QUrl url(profile.serverUrl);
        const QString host = url.host().isEmpty() ? profile.serverUrl : url.host();
        result.push_back(QVariantMap { { QStringLiteral("profileId"), profile.profileId },
            { QStringLiteral("serverName"), profile.serverName }, { QStringLiteral("serverUrl"), profile.serverUrl },
            { QStringLiteral("serverHost"), host }, { QStringLiteral("userName"), profile.userName },
            { QStringLiteral("avatarTag"), profile.avatarTag },
            { QStringLiteral("needsAuthentication"), profile.needsAuthentication || profile.accessToken.isEmpty() } });
    }
    return result;
}

QStringList SessionController::localStorageKeys()
{
    return { QStringLiteral("login/serverUrl"), QStringLiteral("login/username") };
}

QCoro::Task<bool> SessionController::initializeAsync()
{
    StartupState state = co_await m_database->loadStartupStateAsync(localStorageKeys());
    co_return initializeFromStorage(std::move(state.values), std::move(state.profiles));
}

bool SessionController::initializeFromStorage(QVariantMap values, std::vector<AccountProfile> profiles)
{
    m_serverUrl = canonicalServerUrl(values.value(QStringLiteral("login/serverUrl")).toString());
    m_username = values.value(QStringLiteral("login/username")).toString();
    m_profiles = std::move(profiles);
    sortProfiles();

    m_api->setSession({});
    emit serverUrlChanged();
    emit usernameChanged();
    emit accountProfilesChanged();
    if (m_profiles.size() == 1)
        applyStoredProfile(m_profiles.front(), true);
    return !m_profiles.empty();
}

void SessionController::setDiscoveredServers(DiscoveredServerModel *servers)
{
    m_discoveredServers = servers;
}

void SessionController::chooseDiscoveredServer(int index)
{
    if (!m_discoveredServers)
        return;
    const auto server = m_discoveredServers->serverAt(index);
    if (server.address.isEmpty())
        return;
    setServerName(server.name);
    setServerUrl(server.address);
}

void SessionController::rememberServer(const QString& name, const QString& address)
{
    const QString normalizedAddress = address.trimmed();
    if (normalizedAddress.isEmpty())
        return;

    const QString serverName = name.trimmed().isEmpty() ? QStringLiteral("Jellyfin Server") : name.trimmed();
    if (m_discoveredServers) {
        m_discoveredServers->upsertServer({ normalizedAddress, serverName, normalizedAddress });
        emit serverRemembered();
    }
    setServerName(serverName);
    setServerUrl(normalizedAddress);
}

void SessionController::switchUser()
{
    qInfo() << "session: switch user requested";
    emit switchUserRequested();
}

void SessionController::setServerUrl(const QString& serverUrl)
{
    QString normalized = canonicalServerUrl(serverUrl);
    if (normalized.isEmpty()) {
        normalized = serverUrl.trimmed();
        while (normalized.endsWith(QLatin1Char('/')))
            normalized.chop(1);
    }
    if (m_serverUrl == normalized)
        return;
    m_serverUrl = normalized;
    emit serverUrlChanged();
}

void SessionController::setServerName(const QString& serverName)
{
    const QString normalized = serverName.trimmed();
    if (m_serverName == normalized)
        return;
    m_serverName = normalized;
    emit serverNameChanged();
}

void SessionController::setUsername(const QString& username)
{
    if (m_username == username)
        return;
    m_username = username;
    emit usernameChanged();
}

void SessionController::setPassword(const QString& password)
{
    if (m_password == password)
        return;
    m_password = password;
    emit passwordChanged();
}

void SessionController::login()
{
    if (m_serverUrl.isEmpty() || m_username.isEmpty()) {
        emit errorOccurred(QStringLiteral("Server and username are required."));
        return;
    }

    emit busyChanged(true, QStringLiteral("Signing in…"));
    m_api->setServerUrl(m_serverUrl);
    auto authentication = m_api->authenticateByName(m_username, m_password);
    clearPassword();
    Async::runScoped(
        this, std::move(authentication),
        [this](const AuthSession& session) {
            emit busyChanged(false, {});
            activateSession(session, true);
        },
        [this](const std::exception_ptr& error) {
            emit busyChanged(false, {});
            emit errorOccurred(exceptionMessage(error));
        });
}

void SessionController::activateProfile(const QString& profileId)
{
    emit profileActivationStarted();
    emit busyChanged(true, QStringLiteral("Opening profile…"));
    Async::runScoped(
        this, activateProfileAsync(profileId), []() {},
        [this](const std::exception_ptr& error) {
            emit busyChanged(false, {});
            emit errorOccurred(exceptionMessage(error));
        },
        "profile activation");
}

QCoro::Task<void> SessionController::activateProfileAsync(const QString& profileId)
{
    const std::optional<AccountProfile> stored = co_await m_database->activateAccountProfileAsync(profileId);
    emit busyChanged(false, {});
    if (!stored) {
        emit errorOccurred(QStringLiteral("That saved account is no longer available."));
        co_return;
    }
    if (!applyStoredProfile(*stored, false))
        emit errorOccurred(QStringLiteral("Authentication required for this account."));
}

bool SessionController::applyStoredProfile(AccountProfile profile, bool persistUsage)
{
    const bool authenticationRequired = profile.needsAuthentication || profile.accessToken.isEmpty();
    if (persistUsage && !authenticationRequired) {
        profile.lastUsedAt = QDateTime::currentMSecsSinceEpoch();
        m_database->upsertAccountProfile(profile);
    }

    const auto local = std::find_if(m_profiles.begin(), m_profiles.end(),
        [&profile](const AccountProfile& candidate) { return candidate.profileId == profile.profileId; });
    if (local != m_profiles.end())
        *local = profile;
    else
        m_profiles.push_back(profile);
    sortProfiles();
    emit accountProfilesChanged();

    setServerName(profile.serverName);
    setServerUrl(profile.serverUrl);
    setUsername(profile.userName);
    clearPassword();
    if (authenticationRequired) {
        if (!m_profileSignInRequired)
            m_profileSignInRequired = true;
        // This is also the navigation event. Re-emit when an already-expired
        // profile is chosen again after returning to the profile grid.
        emit profileSignInRequiredChanged();
        return false;
    }

    if (m_profileSignInRequired) {
        m_profileSignInRequired = false;
        emit profileSignInRequiredChanged();
    }
    m_activeProfileId = profile.profileId;
    emit activeProfileChanged();
    activateSession({ profile.userId, profile.userName, profile.accessToken, profile.serverId }, false);
    return true;
}

void SessionController::prepareProfileSignIn(const QString& profileId)
{
    const auto it = std::find_if(m_profiles.cbegin(), m_profiles.cend(),
        [&profileId](const AccountProfile& profile) { return profile.profileId == profileId; });
    if (it == m_profiles.cend()) {
        emit errorOccurred(QStringLiteral("That saved account is no longer available."));
        return;
    }
    setProfileSignInFields(*it);
}

void SessionController::setProfileSignInFields(const AccountProfile& profile)
{
    setServerUrl(profile.serverUrl);
    setServerName(profile.serverName);
    setUsername(profile.userName);
    clearPassword();
    if (!m_profileSignInRequired) {
        m_profileSignInRequired = true;
        emit profileSignInRequiredChanged();
    }
}

void SessionController::updateProfileServer(const QString& profileId, const QString& name, const QString& url)
{
    const QString canonicalUrl = canonicalServerUrl(url);
    const auto it = std::find_if(m_profiles.begin(), m_profiles.end(),
        [&profileId](const AccountProfile& profile) { return profile.profileId == profileId; });
    if (it == m_profiles.end() || canonicalUrl.isEmpty()) {
        emit errorOccurred(QStringLiteral("Enter a valid server address."));
        return;
    }
    it->serverName = name.trimmed().isEmpty() ? QStringLiteral("Jellyfin Server") : name.trimmed();
    it->serverUrl = canonicalUrl;
    m_database->upsertAccountProfile(*it);
    if (m_activeProfileId == profileId) {
        setServerName(it->serverName);
        setServerUrl(it->serverUrl);
        m_api->setServerUrl(it->serverUrl);
        emit activeProfileChanged();
    }
    emit accountProfilesChanged();
}

void SessionController::removeProfile(const QString& profileId)
{
    const bool removingActive = profileId == m_activeProfileId;
    const auto oldSize = m_profiles.size();
    std::erase_if(m_profiles, [&profileId](const AccountProfile& profile) { return profile.profileId == profileId; });
    if (m_profiles.size() == oldSize)
        return;
    m_database->removeAccountProfile(profileId);
    emit accountProfilesChanged();
    if (removingActive) {
        clearActiveSession();
        emit loggedOut();
    }
}

void SessionController::clearProfiles()
{
    if (m_profiles.empty())
        return;
    m_profiles.clear();
    m_database->clearAccountProfiles();
    emit accountProfilesChanged();
    clearActiveSession();
    emit loggedOut();
}

void SessionController::deactivate()
{
    clearPassword();
    m_profileSignInRequired = false;
    emit profileSignInRequiredChanged();
    clearActiveSession();
    emit loggedOut();
}

void SessionController::acceptSession(const AuthSession& session)
{
    activateSession(session, true);
}

void SessionController::logout()
{
    qInfo() << "session: logout requested";
    emit logoutStarted();
    clearPassword();
    if (!m_activeProfileId.isEmpty()) {
        const auto it = std::find_if(m_profiles.begin(), m_profiles.end(),
            [this](const AccountProfile& profile) { return profile.profileId == m_activeProfileId; });
        if (it != m_profiles.end()) {
            it->accessToken.clear();
            it->needsAuthentication = true;
            m_database->expireAccountProfile(it->profileId);
            emit accountProfilesChanged();
        }
    }
    clearActiveSession();
    emit loggedOut();
}

void SessionController::expireSession(const QString& message)
{
    if (m_api->session().accessToken.isEmpty())
        return;
    qWarning() << "session: authentication expired";
    const QString expiredProfileId = m_activeProfileId;
    logout();
    if (!expiredProfileId.isEmpty())
        prepareProfileSignIn(expiredProfileId);
    emit errorOccurred(message);
}

bool SessionController::handleUnauthorized(const std::exception_ptr& error)
{
    const QString message = exceptionMessage(error);
    if (!message.contains(QStringLiteral("(401)"))
        && !message.contains(QStringLiteral("Unauthorized"), Qt::CaseInsensitive)) {
        return false;
    }

    if (!m_api->session().accessToken.isEmpty())
        expireSession(QStringLiteral("Your Jellyfin session has expired. Sign in again."));
    return true;
}

void SessionController::activateSession(const AuthSession& session, bool persist)
{
    clearPassword();
    if (session.accessToken.isEmpty()) {
        emit errorOccurred(QStringLiteral("The server returned an empty session."));
        return;
    }

    AuthSession activeSession = session;
    if (activeSession.userName.isEmpty())
        activeSession.userName = m_username;
    if (!activeSession.userName.isEmpty() && m_username != activeSession.userName) {
        m_username = activeSession.userName;
        emit usernameChanged();
    }

    m_api->setServerUrl(m_serverUrl);
    m_api->setSession(activeSession);
    if (m_profileSignInRequired) {
        m_profileSignInRequired = false;
        emit profileSignInRequiredChanged();
    }
    if (persist) {
        m_database->saveLoginHints(m_serverUrl, m_username);
        upsertActiveProfile(activeSession);
    }

    if (m_username.isEmpty() && !activeSession.userId.isEmpty()) {
        Async::runScoped(
            this, m_api->fetchCurrentUserName(),
            [this](const QString& userName) {
                const QString normalized = userName.trimmed();
                if (normalized.isEmpty() || !m_username.isEmpty())
                    return;
                m_username = normalized;
                emit usernameChanged();
                AuthSession refreshed = m_api->session();
                refreshed.userName = normalized;
                m_api->setSession(refreshed);
                m_database->saveLoginHints(m_serverUrl, normalized);
                upsertActiveProfile(refreshed);
            },
            [](const std::exception_ptr& error) {
                qWarning() << "session: user name backfill failed" << exceptionMessage(error);
            });
    }

    emit authenticatedChanged(activeSession);
    emit authenticatedStateChanged();
    Async::runScoped(
        this, m_api->postCapabilities(), []() {},
        [](const std::exception_ptr& error) {
            qWarning() << "session: capability report failed" << exceptionMessage(error);
        });
}

void SessionController::upsertActiveProfile(const AuthSession& session)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString id = accountProfileId(session.serverId, m_serverUrl, session.userId);
    const auto existing = std::find_if(
        m_profiles.begin(), m_profiles.end(), [&id](const AccountProfile& profile) { return profile.profileId == id; });
    const qint64 createdAt = existing == m_profiles.end() ? now : existing->createdAt;
    const QString avatarTag = existing == m_profiles.end() ? QString() : existing->avatarTag;
    AccountProfile profile { id, session.serverId,
        m_serverName.isEmpty() ? QStringLiteral("Jellyfin Server") : m_serverName, canonicalServerUrl(m_serverUrl),
        session.userId, session.userName.isEmpty() ? m_username : session.userName, session.accessToken, avatarTag, now,
        createdAt, false };
    if (existing == m_profiles.end())
        m_profiles.push_back(profile);
    else
        *existing = profile;
    m_activeProfileId = id;
    m_database->upsertAccountProfile(profile);
    sortProfiles();
    emit activeProfileChanged();
    emit accountProfilesChanged();
}

void SessionController::clearActiveSession()
{
    const bool wasAuthenticated = authenticated();
    const bool hadActiveProfile = !m_activeProfileId.isEmpty();
    m_activeProfileId.clear();
    m_api->setSession({});
    clearPassword();
    if (hadActiveProfile)
        emit activeProfileChanged();
    if (wasAuthenticated)
        emit authenticatedStateChanged();
}

void SessionController::clearPassword()
{
    if (m_password.isEmpty())
        return;
    m_password.fill(QLatin1Char('\0'));
    m_password.clear();
    emit passwordChanged();
}

void SessionController::sortProfiles()
{
    std::stable_sort(m_profiles.begin(), m_profiles.end(), [](const AccountProfile& left, const AccountProfile& right) {
        if (left.lastUsedAt != right.lastUsedAt)
            return left.lastUsedAt > right.lastUsedAt;
        return left.createdAt < right.createdAt;
    });
}

} // namespace JellyfinNative
