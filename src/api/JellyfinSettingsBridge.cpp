#include "JellyfinSettingsBridge.h"

#include "../app/SettingsController.h"
#include "../common/AsyncTask.h"
#include "JellyfinApiFacade.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>

namespace JellyfinNative {

JellyfinSettingsBridge::JellyfinSettingsBridge(SettingsController *settings, JellyfinApiFacade *api, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
    , m_api(api)
{
    connect(
        m_settings, &SettingsController::playbackPreferencesChanged, m_api, &JellyfinApiFacade::setPlaybackPreferences);
    connect(m_settings, &SettingsController::remoteControlTargetEnabledChanged, m_api,
        &JellyfinApiFacade::setRemoteControlTargetEnabled);
    connect(m_settings, &SettingsController::remoteLoadRequested, this, &JellyfinSettingsBridge::loadRemote);
    connect(m_settings, &SettingsController::remoteCleared, this, [this]() { m_remoteLoadStarted = false; });
    connect(m_settings, &SettingsController::userConfigurationChanged, this, [this](const QJsonObject& configuration) {
        if (!m_api->signedIn())
            return;
        Async::runScoped(
            this, m_api->updateUserConfiguration(configuration), []() {},
            [this](const std::exception_ptr& error) { emit errorOccurred(exceptionMessage(error)); });
    });
}

void JellyfinSettingsBridge::loadRemote()
{
    if (m_remoteLoadStarted || !m_api->signedIn())
        return;
    m_remoteLoadStarted = true;

    Async::runScoped(
        this, m_api->fetchCultures(), [this](const QJsonArray& cultures) { m_settings->applyRemoteCultures(cultures); },
        [](const std::exception_ptr& error) {
            qWarning() << "subtitles: culture list failed" << exceptionMessage(error);
        });

    Async::runScoped(
        this, m_api->fetchUserConfiguration(),
        [this](const QJsonObject& configuration) { m_settings->applyRemoteUserConfiguration(configuration); },
        [](const std::exception_ptr& error) {
            qWarning() << "subtitles: user configuration failed" << exceptionMessage(error);
        });
}

} // namespace JellyfinNative
