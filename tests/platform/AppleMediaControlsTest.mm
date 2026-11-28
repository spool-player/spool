#include "TestMain.h"
#include "TestRequire.h"
#include "app/RouterController.h"
#include "app/SettingsController.h"
#include "cache/DatabaseManager.h"
#include "platform/NativeAppWindow.h"
#include "platform/PlatformApplicationServices.h"
#include "platform/apple/AppleSystemMediaControls.h"
#include "player/PlayerController.h"

#include <QCoroTask>
#include <QEvent>
#include <QGuiApplication>
#include <QTemporaryDir>

#import <MediaPlayer/MediaPlayer.h>

namespace Spool {
struct AppleMediaControlsTestAccess {
    static void run(ApplicationHooks& hooks)
    {
        using SpoolTests::require;
        auto bridge = std::make_unique<AppleSystemMediaControls>(hooks);
        auto *player = hooks.player;
        auto *commands = MPRemoteCommandCenter.sharedCommandCenter;
        auto *info = MPNowPlayingInfoCenter.defaultCenter;
        const auto invoke = [&](MPRemoteCommand *command) {
            for (NSArray *entry in bridge->m_commands) {
                if (entry[0] != command)
                    continue;
                using Handler = MPRemoteCommandHandlerStatus (^)(MPRemoteCommandEvent *);
                Handler handler = (Handler)entry[2];
                require(handler(nil) == MPRemoteCommandHandlerStatusSuccess,
                    "the registered native callback accepts the command");
                return;
            }
            require(false, "find the handler registered with the native command");
        };
        player->m_sessionActive = true;
        player->m_title = QStringLiteral("Synthetic playback session");
        player->m_paused = false;
        bridge->updateNowPlaying();
        require([info.nowPlayingInfo[MPMediaItemPropertyTitle] isEqualToString:@"Synthetic playback session"],
            "the bridge publishes metadata from the actual player state");
#if defined(Q_OS_MACOS)
        require(info.playbackState == MPNowPlayingPlaybackStatePlaying, "active playback publishes Playing");
#endif
        invoke(commands.pauseCommand);
        require(!player->paused(), "native actions are queued, not invoked on a foreign callback thread");
        QCoreApplication::sendPostedEvents(qGuiApp, QEvent::MetaCall);
        require(player->paused(), "the registered pause callback reaches the player");
#if defined(Q_OS_MACOS)
        require(info.playbackState == MPNowPlayingPlaybackStatePaused, "paused playback publishes Paused");
#endif
        invoke(commands.playCommand);
        QCoreApplication::sendPostedEvents(qGuiApp, QEvent::MetaCall);
        require(!player->paused(), "the registered play callback reaches the player");
        invoke(commands.togglePlayPauseCommand);
        QCoreApplication::sendPostedEvents(qGuiApp, QEvent::MetaCall);
        require(player->paused(), "the registered toggle callback reaches the player");
        player->m_sessionActive = false;
        bridge->updateNowPlaying();
        require(!info.nowPlayingInfo, "stopping a session clears its actual metadata");
#if defined(Q_OS_MACOS)
        require(info.playbackState == MPNowPlayingPlaybackStateStopped, "inactive playback publishes Stopped");
#endif
        player->m_sessionActive = true;
        bridge->updateNowPlaying();
        int next = 0, previous = 0, stops = 0;
        hooks.playNext = [&] { ++next; };
        hooks.playPrevious = [&] { ++previous; };
        hooks.stopPlayback = [&] { ++stops; };
        invoke(commands.nextTrackCommand);
        invoke(commands.previousTrackCommand);
        invoke(commands.stopCommand);
        QCoreApplication::sendPostedEvents(qGuiApp, QEvent::MetaCall);
        require(next == 1 && previous == 1 && stops == 1, "registered navigation and stop handlers reach hooks");
        invoke(commands.nextTrackCommand);
        bridge->disable();
        bridge->syncPreference();
        QCoreApplication::sendPostedEvents(qGuiApp, QEvent::MetaCall);
        require(next == 1, "disable and re-enable cannot revive a queued callback from the old registration");
        invoke(commands.nextTrackCommand);
        bridge.reset();
        QCoreApplication::sendPostedEvents(qGuiApp, QEvent::MetaCall);
        require(next == 1, "destroying the bridge invalidates queued native callbacks");
        player->m_sessionActive = false;
        require(!info.nowPlayingInfo, "withdrawal clears the bridge-produced metadata");
#if defined(Q_OS_MACOS)
        require(info.playbackState == MPNowPlayingPlaybackStateStopped, "withdrawal publishes Stopped");
#endif
        hooks.playNext = {};
        hooks.playPrevious = {};
        hooks.stopPlayback = {};
    }
};
} // namespace Spool

SPOOL_TEST_MAIN("apple-media-controls")
{
    using SpoolTests::require;
#if defined(Q_OS_MACOS)
    qputenv("QT_QPA_PLATFORM", "cocoa");
#endif
    QGuiApplication app(argc, argv);
    QTemporaryDir profile;
    require(profile.isValid(), "create the private media controls profile");
    Spool::DatabaseManager database;
    require(database.initialize(profile.filePath(QStringLiteral("settings.sqlite"))), "open the settings database");
    Spool::SettingsController settings(&database, nullptr, nullptr);
    QCoro::waitFor(settings.loadLocalAsync());
    Spool::NativeAppWindow window(QStringLiteral("media-controls-test"));
    Spool::PlayerController player(&window, nullptr, nullptr, QString());
    Spool::RouterController router;
    Spool::ApplicationHooks hooks;
    hooks.player = &player;
    hooks.settings = &settings;

    const QString key = QStringLiteral("appearance/appleMediaControls");
    MPRemoteCommandCenter *commands = MPRemoteCommandCenter.sharedCommandCenter;
    MPNowPlayingInfoCenter *info = MPNowPlayingInfoCenter.defaultCenter;
    // Detect an actual enable through MediaPlayer, whose commands default to enabled.
    commands.playCommand.enabled = NO;
    commands.changePlaybackPositionCommand.enabled = NO;
    {
        Spool::PlatformApplicationServices services(app, window, hooks, router);
        services.start();
#if defined(Q_OS_MACOS)
        require(!commands.playCommand.enabled && !commands.changePlaybackPositionCommand.enabled,
            "macOS leaves system commands untouched until the user opts in");
        QCoro::waitFor(settings.applyValues({ { key, true } }, Spool::ChangeOrigin::User));
#endif
        require(commands.playCommand.enabled && commands.changePlaybackPositionCommand.enabled,
            "enabling system playback controls registers play and seek commands");
        info.nowPlayingInfo = @ { MPMediaItemPropertyTitle : @"Synthetic Now Playing item" };
        require([info.nowPlayingInfo[MPMediaItemPropertyTitle] isEqualToString:@"Synthetic Now Playing item"],
            "seed the app's native metadata to exercise removal");
        QCoro::waitFor(settings.applyValues({ { key, false } }, Spool::ChangeOrigin::User));
        require(
            !commands.playCommand.enabled && !commands.changePlaybackPositionCommand.enabled && !info.nowPlayingInfo,
            "switching off withdraws system commands and the Now Playing item");
        QCoro::waitFor(settings.applyValues({ { key, true } }, Spool::ChangeOrigin::User));
        require(commands.playCommand.enabled && commands.changePlaybackPositionCommand.enabled,
            "switching back on restores native commands");
    }
    require(!commands.playCommand.enabled && !commands.changePlaybackPositionCommand.enabled,
        "service destruction withdraws native commands");
    Spool::AppleMediaControlsTestAccess::run(hooks);
    return 0;
}
