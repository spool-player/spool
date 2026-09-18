#pragma once

#include <QObject>

namespace JellyfinNative {

class JellyfinApiFacade;
class SettingsController;

// The Jellyfin half of settings: pushes the playback and remote-control
// preferences into the facade, and fetches the account's cultures and user
// configuration when the controller asks for them. The controller itself
// knows nothing about the server.
class JellyfinSettingsBridge final : public QObject {
    Q_OBJECT

public:
    JellyfinSettingsBridge(SettingsController *settings, JellyfinApiFacade *api, QObject *parent = nullptr);

signals:
    void errorOccurred(const QString& message);

private:
    void loadRemote();

    SettingsController *m_settings = nullptr;
    JellyfinApiFacade *m_api = nullptr;
    bool m_remoteLoadStarted = false;
};

} // namespace JellyfinNative
