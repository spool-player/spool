#include "app/SettingsController.h"

#include "app/ArtworkService.h"
#include "cache/DatabaseManager.h"
#include "platform/PlatformSettingsPolicy.h"

#include "RecordingArtworkSource.h"
#include "TestMain.h"

#include <QCoreApplication>
#include <QCoroTask>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

using JellyfinNative::ArtworkService;
using JellyfinNative::DatabaseManager;
using JellyfinNative::MovieItem;
using JellyfinNative::platformDefaultArtworkFormat;
using JellyfinNative::SettingsController;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

} // namespace

JELLYFIN_TEST_MAIN("settings-controller")
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary settings directory was not created");

    DatabaseManager database;
    require(database.initialize(directory.filePath(QStringLiteral("settings.sqlite"))),
        "settings database did not initialize");

    JellyfinNative::Testing::RecordingArtworkSource artworkSource;
    ArtworkService artwork(QString(), 0, 1024, 1, nullptr);
    artwork.setSource(&artworkSource);
    MovieItem poster;
    poster.id = QStringLiteral("item1");
    poster.posterTag = QStringLiteral("tag1");
    const QVariant posterValue = QVariant::fromValue(poster);
    const auto posterUrl = [&] { return artwork.url(posterValue, QStringLiteral("poster")); };

    SettingsController settings(&database, nullptr, &artwork);
    QCoro::waitFor(settings.loadLocalAsync());

    // A fresh profile leaves the codec to the platform, and the artwork
    // service has to be told before the first poster is requested rather than
    // when the settings page is first opened.
    require(posterUrl().contains(QStringLiteral("format=") + QString::fromLatin1(platformDefaultArtworkFormat())),
        "artwork did not start on the platform's default format");
    require(posterUrl().contains(QStringLiteral("quality=75")), "artwork did not start at the default webp quality");

    settings.setValue(QStringLiteral("artwork/format"), QStringLiteral("jpeg"));
    require(posterUrl().contains(QStringLiteral("format=jpeg")), "changing the artwork format did not reach artwork");
    require(posterUrl().contains(QStringLiteral("quality=82")),
        "switching to jpeg did not pick up the jpeg quality default");
    settings.setValue(QStringLiteral("artwork/jpegQuality"), QStringLiteral("90"));
    require(posterUrl().contains(QStringLiteral("quality=90")), "changing the jpeg quality did not reach artwork");
    settings.setValue(QStringLiteral("artwork/format"), QStringLiteral("webp"));
    require(
        posterUrl().contains(QStringLiteral("quality=75")), "switching back to webp did not restore the webp quality");
    require(settings.uiScalePercent() == 100, "desktop UI scale default was not 100 percent");
    require(!settings.value(QStringLiteral("playback/manualStreamingBitrate")).toBool(),
        "fresh profile unexpectedly enabled the manual streaming limit");
    require(!settings.value(QStringLiteral("playback/unlimitedLocalBitrate")).toBool(),
        "fresh profile unexpectedly enabled unlimited local-network playback");
    require(settings.value(QStringLiteral("playback/forwardCacheSizeMiB")).toInt() == 32,
        "fresh profile did not use the 32 MB forward cache default");
    require(settings.value(QStringLiteral("playback/rememberSeriesAudioTrack")).toBool(),
        "fresh profile did not remember per-series audio tracks by default");
    require(settings.playerControlTooltipsEnabled(), "fresh profile unexpectedly hid player control tooltips");
    require(settings.remoteControlTargetEnabled(), "desktop remote-control target default was not enabled");

    settings.setValue(QStringLiteral("playback/forwardCacheSizeMiB"), QStringLiteral("256"));
    require(settings.value(QStringLiteral("playback/forwardCacheSizeMiB")).toString() == QStringLiteral("256"),
        "forward cache size was not updated");
    require(QCoro::waitFor(database.loadSettingAsync(QStringLiteral("playback/forwardCacheSizeMiB")))
            == QStringLiteral("256"),
        "forward cache size was not persisted");
    settings.setValue(QStringLiteral("playback/rememberSeriesAudioTrack"), false);
    require(!settings.value(QStringLiteral("playback/rememberSeriesAudioTrack")).toBool(),
        "series audio-track retention toggle was not updated");
    settings.setValue(QStringLiteral("remote/acceptCommands"), false);
    require(!settings.remoteControlTargetEnabled(), "remote target toggle was not applied");

    settings.setAudioDelayMs(120);
    require(settings.audioDelayMs() == 120, "audio delay setter did not update the global desktop value");
    require(QCoro::waitFor(database.loadSettingAsync(QStringLiteral("settings/audioDelayMs"))) == QStringLiteral("120"),
        "global desktop audio delay was not persisted");

    settings.setUiScalePercent(40);
    require(settings.uiScalePercent() == 50, "UI scale setter did not clamp to its lower bound");
    settings.setUiScalePercent(135);
    require(settings.uiScalePercent() == 135, "UI scale setter did not apply the selected scale");

    require(
        QCoro::waitFor(database.loadSettingAsync(QStringLiteral("appearance/uiScalePercent"))) == QStringLiteral("135"),
        "selected UI scale was not persisted");

    require(!settings.value(QStringLiteral("subtitles/alwaysOverridePositionAndSize")).toBool(),
        "fresh profile unexpectedly enabled subtitle geometry override");
    settings.setValue(QStringLiteral("subtitles/alwaysOverridePositionAndSize"), true);
    require(QCoro::waitFor(database.loadSettingAsync(QStringLiteral("subtitles/alwaysOverridePositionAndSize")))
            == QStringLiteral("true"),
        "subtitle geometry override was not persisted");
    SettingsController restoredSubtitleSettings(&database, nullptr, nullptr, nullptr);
    QCoro::waitFor(restoredSubtitleSettings.loadLocalAsync());
    require(restoredSubtitleSettings.value(QStringLiteral("subtitles/alwaysOverridePositionAndSize")).toBool(),
        "persisted subtitle geometry override was not restored");

    settings.setValue(QStringLiteral("subtitles/styling"), QStringLiteral("Custom"));
    settings.setValue(QStringLiteral("subtitles/scalePercent"), 150);
    settings.setValue(QStringLiteral("subtitles/textColor"), QStringLiteral("#00ff00"));
    settings.setValue(QStringLiteral("subtitles/overrideTextColor"), true);
    settings.setValue(QStringLiteral("subtitles/hdrBrightnessPercent"), 100);
    settings.setValue(QStringLiteral("subtitles/bitmapSharpnessPercent"), 100);
    settings.setValue(QStringLiteral("subtitles/recolorImageSubtitles"), true);
    settings.setValue(QStringLiteral("subtitles/bitmapShadowEnabled"), false);
    settings.setValue(QStringLiteral("subtitles/bitmapShadowSpreadSize"), 16);
    settings.setValue(QStringLiteral("subtitles/allowInBlackBars"), false);
    settings.resetSubtitleAppearance();
    require(settings.value(QStringLiteral("subtitles/styling")).toString() == QStringLiteral("Auto"),
        "subtitle appearance reset did not restore automatic styling");
    require(settings.value(QStringLiteral("subtitles/scalePercent")).toInt() == 100,
        "subtitle appearance reset did not restore normal text size");
    require(settings.value(QStringLiteral("subtitles/textColor")).toString() == QStringLiteral("#ffffff"),
        "subtitle appearance reset did not restore white text");
    require(!settings.value(QStringLiteral("subtitles/overrideTextColor")).toBool(),
        "subtitle appearance reset did not restore authored text colours");
    require(settings.value(QStringLiteral("subtitles/bitmapSharpnessPercent")).toInt() == 45,
        "subtitle appearance reset did not restore slightly smooth image subtitle edges");
    require(settings.value(QStringLiteral("subtitles/hdrBrightnessPercent")).toInt() == 50,
        "subtitle appearance reset did not restore HDR subtitle brightness");
    require(!settings.value(QStringLiteral("subtitles/recolorImageSubtitles")).toBool(),
        "subtitle appearance reset did not restore original image colours");
    require(settings.value(QStringLiteral("subtitles/bitmapShadowEnabled")).toBool(),
        "subtitle appearance reset did not restore the image subtitle shadow");
    require(settings.value(QStringLiteral("subtitles/bitmapShadowSpreadSize")).toInt() == 6,
        "subtitle appearance reset did not restore the mild wide-shadow size");
    require(settings.value(QStringLiteral("subtitles/allowInBlackBars")).toBool(),
        "subtitle appearance reset did not restore black-bar placement");
    require(!settings.value(QStringLiteral("subtitles/alwaysOverridePositionAndSize")).toBool(),
        "subtitle appearance reset did not disable geometry override");
    require(QCoro::waitFor(database.loadSettingAsync(QStringLiteral("subtitles/alwaysOverridePositionAndSize")))
            == QStringLiteral("false"),
        "subtitle appearance reset did not persist disabled geometry override");

    settings.completePlayerControlTooltipSession();
    settings.completePlayerControlTooltipSession();
    settings.completePlayerControlTooltipSession();
    require(
        !settings.playerControlTooltipsEnabled(), "control tooltips remained enabled after three playback sessions");
    require(QCoro::waitFor(database.loadSettingAsync(QStringLiteral("player/controlTooltipSessions")))
            == QStringLiteral("3"),
        "completed control-tooltip sessions were not persisted");

    SettingsController restored(&database, nullptr, nullptr);
    QCoro::waitFor(restored.loadLocalAsync());
    require(restored.uiScalePercent() == 135, "persisted UI scale was not restored");
    require(restored.audioDelayMs() == 120, "persisted global desktop audio delay was not restored");
    require(restored.value(QStringLiteral("playback/forwardCacheSizeMiB")).toString() == QStringLiteral("256"),
        "persisted forward cache size was not restored");
    require(!restored.value(QStringLiteral("playback/rememberSeriesAudioTrack")).toBool(),
        "series audio-track retention toggle was not restored");
    require(!restored.remoteControlTargetEnabled(), "persisted remote target toggle was not restored");
    require(!restored.playerControlTooltipsEnabled(), "persisted control-tooltip sessions were not restored");

    database.shutdown();
    return EXIT_SUCCESS;
}
