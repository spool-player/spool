#pragma once

#include "../common/RequestGeneration.h"
#include "../media/MediaTypes.h"
#include "../platform/MpvConfigPolicy.h"

#include <QCoroTask>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace JellyfinNative {

class DatabaseManager;
class ArtworkService;
class JellyfinApiFacade;
class PlayerController;
struct SettingSpec;

class SettingsController final : public QObject {
    Q_OBJECT
    // Everything the settings pages read and write goes through the generic
    // schema API below (values, setValue, settingsSchema). The named properties
    // here exist only for the handful of places that need one setting directly:
    // the player overlay, the shell zoom shortcut and the Metrics singleton.
    Q_PROPERTY(bool nightModeEnabled MEMBER m_nightModeEnabled WRITE setNightModeEnabled NOTIFY nightModeChanged)
    Q_PROPERTY(int audioDelayMs READ audioDelayMs WRITE setAudioDelayMs NOTIFY audioDelayChanged)
    Q_PROPERTY(int displayLatencyMs READ displayLatencyMs NOTIFY audioOutputDeviceChanged)
    Q_PROPERTY(QString audioDelayTargetLabel READ audioDelayTargetLabel NOTIFY audioOutputDeviceChanged)
    Q_PROPERTY(int uiScalePercent READ uiScalePercent WRITE setUiScalePercent NOTIFY appearanceChanged)
    Q_PROPERTY(QStringList subtitleLanguageOptions READ subtitleLanguageOptions NOTIFY subtitleSettingsChanged)
    Q_PROPERTY(int subtitleLanguageIndex READ subtitleLanguageIndex WRITE setSubtitleLanguageIndex NOTIFY
            subtitleSettingsChanged)
    Q_PROPERTY(QString redButtonAction MEMBER m_redButtonAction NOTIFY buttonRemapChanged)
    Q_PROPERTY(QString greenButtonAction MEMBER m_greenButtonAction NOTIFY buttonRemapChanged)
    Q_PROPERTY(QString yellowButtonAction MEMBER m_yellowButtonAction NOTIFY buttonRemapChanged)
    Q_PROPERTY(QString blueButtonAction MEMBER m_blueButtonAction NOTIFY buttonRemapChanged)
    Q_PROPERTY(QStringList systemSubtitleFonts READ systemSubtitleFonts CONSTANT)
    Q_PROPERTY(QVariantList settingsSchema READ settingsSchema CONSTANT)
    Q_PROPERTY(QVariantMap values READ values NOTIFY settingsValuesChanged)
    Q_PROPERTY(
        bool playerControlTooltipsEnabled READ playerControlTooltipsEnabled NOTIFY playerControlTooltipsEnabledChanged)
    Q_PROPERTY(bool castButtonEnabled READ castButtonEnabled NOTIFY remoteControlSettingsChanged)
    Q_PROPERTY(bool remoteControlTargetEnabled READ remoteControlTargetEnabled NOTIFY remoteControlSettingsChanged)

public:
    // Emitted whenever the stored value is applied, including the first time
    // settings load. Whoever can actually install an update connects to it;
    // this class deliberately knows nothing about platform updaters.
    Q_SIGNAL void automaticUpdatesChanged(bool enabled);

    // Drops the picture-quality setting one rung and reports the name it
    // landed on, or an empty string if it was already at the bottom or the
    // viewer has asked to be left alone. Persisted like any other change, so
    // the next playback starts where this one ended up.
    QString stepDownRenderQuality();

    SettingsController(DatabaseManager *database, JellyfinApiFacade *api, PlayerController *player,
        ArtworkService *artwork, QObject *parent = nullptr);

    int uiScalePercent() const
    {
        return m_uiScalePercent;
    }
    int audioDelayMs() const
    {
        return m_audioDelayMs;
    }
    int displayLatencyMs() const
    {
        return m_displayLatencyMs;
    }
    QString audioDelayTargetLabel() const;
    int subtitleLanguageIndex() const;

    QStringList subtitleLanguageOptions() const;
    QStringList systemSubtitleFonts() const;
    QVariantList settingsSchema() const;
    QVariantMap values() const;
    Q_INVOKABLE QVariant value(const QString& key) const;
    bool playerControlTooltipsEnabled() const
    {
        return m_playerControlTooltipSessions < 3;
    }
    bool castButtonEnabled() const
    {
        return m_castButtonEnabled;
    }
    bool remoteControlTargetEnabled() const
    {
        return m_remoteControlTargetEnabled;
    }

    static QStringList localSettingKeys();
    void applyLocalValues(const QVariantMap& storedValues);

    QCoro::Task<void> loadLocalAsync();
    Q_INVOKABLE void loadRemote();
    void clearRemote();
    Q_INVOKABLE void setValue(const QString& key, const QVariant& value);
    Q_INVOKABLE void previewValue(const QString& key, const QVariant& value);
    Q_INVOKABLE void completePlayerControlTooltipSession();
    Q_INVOKABLE void setNightModeEnabled(bool enabled);
    Q_INVOKABLE void setAudioDelayMs(int delayMs);
    Q_INVOKABLE void setUiScalePercent(int percent);
    Q_INVOKABLE void setSubtitleLanguageIndex(int index);

    Q_INVOKABLE void resetSubtitleAppearance();
    QString mpvConfigMode() const
    {
        return m_mpvConfigMode;
    }
    QString mpvConfigDirectory() const
    {
        return m_mpvConfigDirectory;
    }
    void updateAudioOutputRoute(const QString& output, int displayLatencyMs, int outputLatencyMs);

signals:
    void nightModeChanged();
    void audioDelayChanged();
    void audioOutputDeviceChanged();
    void subtitleSettingsChanged();
    void buttonRemapChanged();
    void appearanceChanged();
    void settingChanged(const QString& key);
    void settingsValuesChanged();
    void playerControlTooltipsEnabledChanged();
    void remoteControlSettingsChanged();
    void errorOccurred(const QString& message);

private:
    bool setSchemaValue(const SettingSpec& spec, const QVariant& value, bool persist, bool apply, bool notify);
    void applySchemaValue(const SettingSpec& spec, const QVariant& value, bool apply);
    void emitSchemaSignals(const SettingSpec& spec);
    void applyPlaybackPreferences();
    void applyArtworkEncoding();
    void applyAudioDelayToPlayer();
    void loadCurrentAudioDelay();
    void applyLoadedAudioDelay(const QString& output, int delayMs);
    void saveSubtitleUserConfiguration();
    void applySubtitlePreferencesToPlayer();
    void applyMpvConfigPolicy();

    DatabaseManager *m_database = nullptr;
    JellyfinApiFacade *m_api = nullptr;
    PlayerController *m_player = nullptr;
    ArtworkService *m_artwork = nullptr;
    QString m_artworkFormat = QStringLiteral("auto");
    int m_artworkWebpQuality = 75;
    int m_artworkJpegQuality = 82;
    QVariantMap m_values;
    bool m_remoteLoadStarted = false;
    bool m_nightModeEnabled = false;
    bool m_castButtonEnabled = true;
    bool m_remoteControlTargetEnabled = true;
    bool m_toneMappingVisualizationEnabled = false;
    int m_maxStreamingHeight = 0;
    bool m_manualStreamingBitrate = false;
    int m_maxStreamingBitrateMbps = 120;
    bool m_unlimitedLocalBitrate = false;
    bool m_preferRemux = true;
    int m_forwardCacheSizeMiB = 32;
    int m_audioDelayMs = 0;
    int m_automaticAudioDelayMs = 0;
    int m_displayLatencyMs = 0;
    QString m_currentAudioOutput;
    bool m_localSettingsLoaded = false;
    RequestGeneration m_audioOutputLoadGeneration;
    QString m_audioOutputMode = QStringLiteral("auto");
    QString m_renderQuality = QStringLiteral("balanced");
    bool m_autoAdjustRenderQuality = true;
    QString m_videoOutputMode = QStringLiteral("enhanced");
    QString m_mpvConfigMode = QStringLiteral("disabled");
    QString m_mpvConfigDirectory;
    int m_uiScalePercent;
    int m_playerControlTooltipSessions = 0;
    SubtitlePreferences m_subtitlePreferences;
    QStringList m_subtitleLanguageCodes { QString() };
    QStringList m_subtitleLanguageLabels { QStringLiteral("Any language") };
    QJsonObject m_userConfiguration;
    QString m_redButtonAction = QStringLiteral("none");
    QString m_greenButtonAction = QStringLiteral("skipBackAndEnableSubs");
    QString m_yellowButtonAction = QStringLiteral("none");
    QString m_blueButtonAction = QStringLiteral("none");
};

} // namespace JellyfinNative
