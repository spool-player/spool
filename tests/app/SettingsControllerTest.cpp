#include "app/SettingsController.h"

#include "app/ArtworkService.h"
#include "app/LocalizationManager.h"
#include "cache/DatabaseManager.h"
#include "diagnostics/InputLatencyMonitor.h"
#include "platform/PlatformSettingsPolicy.h"

#include "RecordingArtworkSource.h"
#include "TestMain.h"

#include <QCoreApplication>
#include <QCoroTask>
#include <QJsonDocument>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

using Spool::ArtworkService;
using Spool::DatabaseManager;
using Spool::MovieItem;
using Spool::platformDefaultArtworkFormat;
using Spool::SettingsController;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

} // namespace

SPOOL_TEST_MAIN("settings-controller")
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary settings directory was not created");
    QCoreApplication::setOrganizationName(QStringLiteral("SpoolTests"));
    QCoreApplication::setApplicationName(QStringLiteral("SettingsController"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());

    DatabaseManager database;
    require(database.initialize(directory.filePath(QStringLiteral("settings.sqlite"))),
        "settings database did not initialize");

    Spool::Testing::RecordingArtworkSource artworkSource;
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

    int commits = 0;
    int notifications = 0;
    int subtitleNotifications = 0;
    QVariantMap committed;
    QObject::connect(&settings, &SettingsController::userValuesCommitted, &settings, [&](QVariantMap values) {
        ++commits;
        committed = std::move(values);
    });
    QObject::connect(&settings, &SettingsController::settingsValuesChanged, &settings, [&] { ++notifications; });
    QObject::connect(
        &settings, &SettingsController::subtitleSettingsChanged, &settings, [&] { ++subtitleNotifications; });
    QCoro::waitFor(settings.applyValues({ { QStringLiteral("subtitles/scalePercent"), 150 },
                                            { QStringLiteral("subtitles/verticalPositionPercent"), 75 },
                                            { QStringLiteral("appearance/uiScalePercent"), 160 },
                                            { QStringLiteral("playback/maxStreamingBitrateMbps"), 5000 } },
        Spool::ChangeOrigin::RemoteSync));
    require(notifications == 1 && subtitleNotifications == 1 && commits == 0,
        "a remote batch must notify once without echoing a user commit");
    require(settings.uiScalePercent() == 135
            && settings.value(QStringLiteral("playback/maxStreamingBitrateMbps")).toInt() == 120,
        "remote application changed a Never key or clamped an unknown remote value");
    QCoro::waitFor(settings.applyValues({ { QStringLiteral("settings/audioDelayMs"), 9000 },
                                            { QStringLiteral("audio/language"), QStringLiteral("en") } },
        Spool::ChangeOrigin::User));
    require(commits == 1 && committed.value(QStringLiteral("settings/audioDelayMs")).toInt() == 2000
            && committed.value(QStringLiteral("audio/language")).toString() == QStringLiteral("eng"),
        "user commit did not expose the exact normalized durable values");
    QCoro::waitFor(settings.applyValues(
        { { QStringLiteral("playback/renderQuality"), QStringLiteral("fast") } }, Spool::ChangeOrigin::Automatic));
    settings.previewValue(QStringLiteral("subtitles/scalePercent"), 125);
    require(commits == 1, "preview or automatic adaptation echoed an upload");
    settings.updateAudioOutputRoute(QStringLiteral("hdmi"), 20, 10);
    if (!Spool::platformUsesPerOutputAudioDelay())
        require(QCoro::waitFor(database.loadSettingAsync(QStringLiteral("settings/audioDelayMs")))
                == QStringLiteral("2000"),
            "route changes rewrote the global audio trim");
    else {
        QCoro::waitFor(
            settings.applyValues({ { QStringLiteral("settings/audioDelayMs"), 150 } }, Spool::ChangeOrigin::User));
        require(QCoro::waitFor(database.loadSettingAsync(Spool::platformAudioDelayStorageKey(QStringLiteral("hdmi"))))
                == QStringLiteral("150"),
            "per-output trim did not persist on the captured route");
    }

    Spool::LocalizationManager locale;
    Spool::InputLatencyMonitor latency;
    settings.attachLocalization(&locale);
    settings.attachInputLatency(&latency);
    QCoro::waitFor(settings.applyValues(
        { { QStringLiteral("i18n/locale"), QStringLiteral("en-GB") }, { QStringLiteral("shell/latencyGuard"), false } },
        Spool::ChangeOrigin::User));
    require(locale.currentLocale() == QStringLiteral("en-GB") && !latency.enabled(),
        "facade did not apply preferences through canonical external owners");
    require(QCoro::waitFor(database.loadSettingAsync(QStringLiteral("i18n/locale"))).isEmpty(),
        "locale was duplicated into the SQLite settings store");
    const int afterExternalCommit = commits;
    latency.setOverlayEnabled(!latency.overlayEnabled());
    require(settings.value(QStringLiteral("shell/latencyOverlay")).toBool() == latency.overlayEnabled()
            && commits == afterExternalCommit,
        "internal latency changes did not mirror without upload");

    const QString journalKey = QStringLiteral("settings/application/i18n/locale");
    const auto seedApplication = [&](const QString& value, Spool::ChangeOrigin origin) {
        const QVariantMap record { { QStringLiteral("value"), value },
            { QStringLiteral("origin"), static_cast<int>(origin) },
            { QStringLiteral("generation"), QStringLiteral("7000") },
            { QStringLiteral("accountId"), QStringLiteral("removed-account") }, { QStringLiteral("deferred"), false } };
        QCoro::waitFor(database.saveSettings(
            { { journalKey, QString::fromUtf8(QJsonDocument::fromVariant(record).toJson(QJsonDocument::Compact)) } }));
    };
    seedApplication(QStringLiteral("en-US"), Spool::ChangeOrigin::User);
    SettingsController recovery(&database, nullptr, nullptr);
    recovery.attachLocalization(&locale);
    QCoro::waitFor(recovery.loadLocalAsync());
    int recoveryCommits = 0;
    QObject::connect(
        &recovery, &SettingsController::userValuesCommitted, &recovery, [&](QVariantMap) { ++recoveryCommits; });
    QCoro::waitFor(recovery.recoverUnfinishedApplications());
    require(locale.currentLocale() == QStringLiteral("en-US") && recoveryCommits == 0
            && QCoro::waitFor(database.loadSettingAsync(journalKey)).isEmpty(),
        "interrupted external application did not recover exactly once without upload");

    seedApplication(QStringLiteral("en-GB"), Spool::ChangeOrigin::User);
    QCoro::waitFor(recovery.loadLocalAsync());
    QCoro::waitFor(recovery.applyValues(
        { { QStringLiteral("i18n/locale"), QStringLiteral("en-AU") } }, Spool::ChangeOrigin::User));
    QCoro::waitFor(recovery.recoverUnfinishedApplications());
    require(locale.currentLocale() == QStringLiteral("en-AU"),
        "unfinished application overrode a later explicit local commit");

    seedApplication(QStringLiteral("en-GB"), Spool::ChangeOrigin::RemoteSync);
    QCoro::waitFor(recovery.loadLocalAsync());
    QCoro::waitFor(recovery.recoverUnfinishedApplications());
    require(locale.currentLocale() == QStringLiteral("en-AU"),
        "invalidated source's unfinished remote locale overrode the local locale");
    const int beforeRace = recoveryCommits;
    auto remoteLocale = recovery.applyValues(
        { { QStringLiteral("i18n/locale"), QStringLiteral("en-US") } }, Spool::ChangeOrigin::RemoteSync);
    recovery.cancelRemoteApplications();
    auto localLocale = recovery.applyValues(
        { { QStringLiteral("i18n/locale"), QStringLiteral("en-GB") } }, Spool::ChangeOrigin::User);
    QCoro::waitFor(std::move(remoteLocale));
    QCoro::waitFor(std::move(localLocale));
    require(locale.currentLocale() == QStringLiteral("en-GB") && recoveryCommits == beforeRace + 1,
        "late external remote application defeated cancellation/local supersession");
    SettingsController noReplay(&database, nullptr, nullptr);
    noReplay.attachLocalization(&locale);
    QCoro::waitFor(noReplay.loadLocalAsync());
    locale.setLocale(QStringLiteral("en-AU"));
    QCoro::waitFor(noReplay.recoverUnfinishedApplications());
    require(locale.currentLocale() == QStringLiteral("en-AU"),
        "startup replayed a replica rather than only unfinished application records");
    const QString trackJournal = QStringLiteral("settings/application/audio/language");
    const QVariantMap deferredTrack { { QStringLiteral("value"), QStringLiteral("fra") },
        { QStringLiteral("origin"), static_cast<int>(Spool::ChangeOrigin::RemoteSync) },
        { QStringLiteral("generation"), QStringLiteral("8000") },
        { QStringLiteral("accountId"), QStringLiteral("account") }, { QStringLiteral("deferred"), true } };
    QCoro::waitFor(database.saveSettings({ { trackJournal,
        QString::fromUtf8(QJsonDocument::fromVariant(deferredTrack).toJson(QJsonDocument::Compact)) } }));
    QCoro::waitFor(recovery.loadLocalAsync());
    const int beforeTrackCommit = recoveryCommits;
    // Even explicitly recommitting the current local preference must discard
    // a remotely queued default for the next item.
    QCoro::waitFor(recovery.applyValues(
        { { QStringLiteral("audio/language"), QStringLiteral("eng") } }, Spool::ChangeOrigin::User));
    QCoro::waitFor(recovery.applyDeferredTrackDefaults());
    require(recovery.value(QStringLiteral("audio/language")) == QStringLiteral("eng")
            && recoveryCommits == beforeTrackCommit + 1
            && QCoro::waitFor(database.loadSettingAsync(trackJournal)).isEmpty(),
        "deferred track default defeated a later explicit local preference");

    {
        QSqlDatabase fault = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("facade-fault"));
        fault.setDatabaseName(directory.filePath(QStringLiteral("state.sqlite")));
        require(fault.open(), "facade fault connection should open");
        QSqlQuery query(fault);
        require(query.exec(QStringLiteral("CREATE TRIGGER reject_facade BEFORE INSERT ON kv "
                                          "WHEN NEW.key = 'theme/accent' BEGIN SELECT RAISE(ABORT, 'fault'); END")),
            "facade persistence fault should install");
        bool failed = false;
        try {
            QCoro::waitFor(recovery.applyValues({ { QStringLiteral("theme/accent"), QStringLiteral("1") } },
                Spool::ChangeOrigin::User,
                { { QStringLiteral("settingsSync/state/failure-test"), QStringLiteral("pending") } }));
        } catch (const std::exception&) {
            failed = true;
        }
        require(failed
                && QCoro::waitFor(database.loadSettingAsync(QStringLiteral("settingsSync/state/failure-test")))
                    .isEmpty(),
            "facade failure left durable intent without its value");
        require(query.exec(QStringLiteral("DROP TRIGGER reject_facade")), "facade persistence fault should remove");
        const int beforeRetry = recoveryCommits;
        recovery.cancelRemoteApplications({}, false);
        QCoro::waitFor(recovery.retryPendingPersistence());
        require(QCoro::waitFor(database.loadSettingAsync(QStringLiteral("theme/accent"))) == QStringLiteral("1")
                && QCoro::waitFor(database.loadSettingAsync(QStringLiteral("settingsSync/state/failure-test")))
                    == QStringLiteral("pending")
                && recoveryCommits == beforeRetry,
            "failed transaction recovery did not persist the exact value/intent without an upload echo");
        fault.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("facade-fault"));
    database.shutdown();
    return EXIT_SUCCESS;
}
