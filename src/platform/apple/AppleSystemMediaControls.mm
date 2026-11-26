#include "AppleSystemMediaControls.h"

#include "app/SettingsController.h"
#include "platform/PlatformApplicationServices.h"
#include "player/PlayerController.h"

#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>

#import <MediaPlayer/MediaPlayer.h>

namespace Spool {

AppleSystemMediaControls::AppleSystemMediaControls(ApplicationHooks& hooks)
    : m_hooks(hooks)
    , m_commands([NSMutableArray array])
{
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &AppleSystemMediaControls::updateNowPlaying);
    connect(m_hooks.player, &PlayerController::playbackStateChanged, this, &AppleSystemMediaControls::updateNowPlaying);
    connect(
        m_hooks.settings, &SettingsController::settingsValuesChanged, this, &AppleSystemMediaControls::syncPreference);
    syncPreference();
}

AppleSystemMediaControls::~AppleSystemMediaControls()
{
    disable();
}

void AppleSystemMediaControls::updateNowPlaying()
{
    if (!m_registration)
        return;
    const auto *player = m_hooks.player;
    if (!player->sessionActive()) {
        MPNowPlayingInfoCenter.defaultCenter.nowPlayingInfo = nil;
        return;
    }
    MPNowPlayingInfoCenter.defaultCenter.nowPlayingInfo = @{
        MPMediaItemPropertyTitle : [NSString stringWithUTF8String:player->title().toUtf8().constData()],
        MPMediaItemPropertyPlaybackDuration : @(player->durationSeconds()),
        MPNowPlayingInfoPropertyElapsedPlaybackTime : @(player->estimatedPositionSeconds()),
        MPNowPlayingInfoPropertyPlaybackRate : @(player->paused() ? 0.0 : player->effectivePlaybackSpeed()),
        MPNowPlayingInfoPropertyIsLiveStream : @(player->durationSeconds() <= 0),
    };
}

void AppleSystemMediaControls::installCommand(
    MPRemoteCommand *command, std::function<void(MPRemoteCommandEvent *)> action)
{
    QPointer<AppleSystemMediaControls> receiver(this);
    const auto registration = m_registration;
    id token = [command addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *event) {
        if (!registration->load(std::memory_order_acquire))
            return MPRemoteCommandHandlerStatusCommandFailed;
        QMetaObject::invokeMethod(
            qGuiApp,
            [receiver, registration, action, event] {
                if (receiver && registration->load(std::memory_order_acquire))
                    action(event);
            },
            Qt::QueuedConnection);
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    [m_commands addObject:@[ command, token ]];
    command.enabled = YES;
}

void AppleSystemMediaControls::disable()
{
    if (!m_registration)
        return;
    // Reject callbacks already queued on the GUI thread before removing targets.
    m_registration->store(false, std::memory_order_release);
    m_timer.stop();
    for (NSArray *registration in m_commands) {
        auto *remote = (MPRemoteCommand *)registration[0];
        remote.enabled = NO;
        [remote removeTarget:registration[1]];
    }
    [m_commands removeAllObjects];
    m_registration.reset();
    MPNowPlayingInfoCenter.defaultCenter.nowPlayingInfo = nil;
}

void AppleSystemMediaControls::syncPreference()
{
    if (!m_hooks.settings->value(QStringLiteral("appearance/appleMediaControls")).toBool()) {
        disable();
        return;
    }
    if (m_registration)
        return;

    m_registration = std::make_shared<std::atomic_bool>(true);
    auto *center = MPRemoteCommandCenter.sharedCommandCenter;
    auto *player = m_hooks.player;
    installCommand(center.playCommand, [player](auto *) { player->setPaused(false); });
    installCommand(center.pauseCommand, [player](auto *) { player->setPaused(true); });
    installCommand(center.togglePlayPauseCommand, [player](auto *) { player->togglePause(); });
    installCommand(center.stopCommand, [this](auto *) {
        if (m_hooks.stopPlayback)
            m_hooks.stopPlayback();
    });
    installCommand(center.nextTrackCommand, [this](auto *) {
        if (m_hooks.playNext)
            m_hooks.playNext();
    });
    installCommand(center.previousTrackCommand, [this](auto *) {
        if (m_hooks.playPrevious)
            m_hooks.playPrevious();
    });
    installCommand(center.changePlaybackPositionCommand, [player](MPRemoteCommandEvent *event) {
        player->seek([(MPChangePlaybackPositionCommandEvent *)event positionTime]);
    });
    m_timer.start();
    updateNowPlaying();
}

} // namespace Spool
