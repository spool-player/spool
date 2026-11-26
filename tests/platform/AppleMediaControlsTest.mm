#include "TestMain.h"
#include "TestRequire.h"
#include "app/RouterController.h"
#include "app/SettingsController.h"
#include "cache/DatabaseManager.h"
#include "platform/NativeAppWindow.h"
#include "platform/PlatformApplicationServices.h"
#include "player/PlayerController.h"

#include <QCoroTask>
#include <QGuiApplication>
#include <QTemporaryDir>

#import <MediaPlayer/MediaPlayer.h>

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
    return 0;
}
