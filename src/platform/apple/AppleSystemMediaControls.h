#pragma once

#include <QObject>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>

@class MPRemoteCommand;
@class MPRemoteCommandEvent;
@class NSMutableArray;

namespace Spool {

class ApplicationHooks;

class AppleSystemMediaControls final : public QObject {
public:
    explicit AppleSystemMediaControls(ApplicationHooks& hooks);
    ~AppleSystemMediaControls() override;

    void updateNowPlaying();

private:
    friend struct AppleMediaControlsTestAccess;
    void syncPreference();
    void disable();
    void installCommand(MPRemoteCommand *command, std::function<void(MPRemoteCommandEvent *)> action);

    ApplicationHooks& m_hooks;
    NSMutableArray *m_commands;
    std::shared_ptr<std::atomic_bool> m_registration;
    QTimer m_timer;
};

} // namespace Spool
