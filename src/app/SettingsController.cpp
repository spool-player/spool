#include "SettingsController.h"

#include "../api/JellyfinApiFacade.h"
#include "../cache/DatabaseManager.h"
#include "../common/AsyncTask.h"
#include "../platform/PlatformSettingsPolicy.h"
#include "../player/PlayerController.h"
#include "../player/RenderTargetProfile.h"
#include "ArtworkService.h"
#include "SettingsSchema.h"

#include <QDebug>
#include <QJsonArray>
#include <QSet>

#include <iterator>

namespace JellyfinNative {

namespace {

    constexpr auto kPlayerControlTooltipSessionsKey = "player/controlTooltipSessions";
    constexpr int kPlayerControlTooltipSessionLimit = 3;

    QString keyString(const SettingSpec& spec)
    {
        return QString::fromLatin1(spec.key);
    }

    const SettingSpec& specForKey(const char *key)
    {
        const SettingSpec *spec = findSettingSpec(QString::fromLatin1(key));
        Q_ASSERT(spec);
        return *spec;
    }

} // namespace

SettingsController::SettingsController(DatabaseManager *database, JellyfinApiFacade *api, PlayerController *player,
    ArtworkService *artwork, QObject *parent)
    : QObject(parent)
    , m_database(database)
    , m_api(api)
    , m_player(player)
    , m_artwork(artwork)
    , m_uiScalePercent(platformDefaultUiScalePercent())
{
}

QStringList SettingsController::subtitleLanguageOptions() const
{
    return m_subtitleLanguageLabels;
}
int SettingsController::subtitleLanguageIndex() const
{
    const int index = m_subtitleLanguageCodes.indexOf(m_subtitlePreferences.language);
    return index >= 0 ? index : 0;
}

QStringList SettingsController::systemSubtitleFonts() const
{
    // Both CONSTANT list properties below must be cached: QML re-invokes the
    // READ accessor for every element access on value-type sequences, so an
    // uncached build turns a JS iteration into a full rebuild per element.
    static const QStringList families = platformSystemSubtitleFonts();
    return families;
}

QVariantList SettingsController::settingsSchema() const
{
    static const QVariantList schema = settingSchemaModel();
    return schema;
}

QVariantMap SettingsController::values() const
{
    return m_values;
}

QVariant SettingsController::value(const QString& key) const
{
    if (m_values.contains(key))
        return m_values.value(key);
    if (const SettingSpec *spec = findSettingSpec(key))
        return settingDefaultValue(*spec);
    return {};
}

QString SettingsController::audioDelayTargetLabel() const
{
    return platformAudioRouteDisplayName(m_currentAudioOutput);
}

QStringList SettingsController::localSettingKeys()
{
    QStringList keys;
    keys.reserve(static_cast<qsizetype>(settingSpecs().size()) + 1);
    for (const SettingSpec& spec : settingSpecs()) {
        if (!spec.persisted)
            continue;
        if (platformUsesPerOutputAudioDelay() && spec.target == SettingTarget::AudioDelay)
            continue;
        keys.append(keyString(spec));
    }
    keys.append(QString::fromLatin1(kPlayerControlTooltipSessionsKey));
    return keys;
}

QCoro::Task<void> SettingsController::loadLocalAsync()
{
    applyLocalValues(co_await m_database->loadValuesAsync(localSettingKeys()));
}

QString SettingsController::stepDownRenderQuality()
{
    if (!m_autoAdjustRenderQuality)
        return {};
    // Ordered worst-effort-last; there is nowhere to go from the bottom.
    static constexpr const char *rungs[] = { "maximum", "high", "balanced", "fast" };
    constexpr int rungCount = static_cast<int>(std::size(rungs));
    int current = rungCount - 1;
    for (int index = 0; index < rungCount; ++index) {
        if (m_renderQuality == QLatin1String(rungs[index])) {
            current = index;
            break;
        }
    }
    if (current >= rungCount - 1) {
        // Nothing cheaper to render with. On a platform that can hand the
        // decoder its own surface, the next step is to stop rendering
        // ourselves at all.
        if (m_videoOutputMode == QLatin1String("direct") || !platformSupportsDirectVideoOutput())
            return {};
        setValue(QStringLiteral("playback/videoOutput"), QStringLiteral("direct"));
        return QStringLiteral("direct");
    }
    const QString next = QString::fromLatin1(rungs[current + 1]);
    setValue(QStringLiteral("playback/renderQuality"), next);
    return next;
}

void SettingsController::applyLocalValues(const QVariantMap& storedValues)
{

    for (const SettingSpec& spec : settingSpecs()) {
        if (!spec.persisted)
            continue;
        const QString key = keyString(spec);
        if (platformUsesPerOutputAudioDelay() && spec.target == SettingTarget::AudioDelay) {
            const QVariant normalized = normalizedSettingValue(spec, settingDefaultValue(spec));
            m_values.insert(key, normalized);
            applySchemaValue(spec, normalized, false);
            continue;
        }
        const QVariant rawValue = storedValues.value(key);
        QVariant defaultValue = settingDefaultValue(spec);
        if (spec.target == SettingTarget::CastButtonEnabled)
            defaultValue = platformDefaultCastButtonEnabled();
        else if (spec.target == SettingTarget::RemoteControlTargetEnabled)
            defaultValue = platformDefaultRemoteControlTargetEnabled();
        else if (spec.target == SettingTarget::RenderQuality)
            defaultValue = QString::fromLatin1(platformDefaultRenderQuality());
        else if (spec.target == SettingTarget::VideoOutputMode)
            defaultValue = QString::fromLatin1(platformDefaultVideoOutput());
        const QVariant stored = !rawValue.isValid() || rawValue.toString().isEmpty() ? defaultValue : rawValue;
        const QVariant normalized = normalizedSettingValue(spec, stored);
        m_values.insert(key, normalized);
        applySchemaValue(spec, normalized, false);

        const QString serialized = serializedSettingValue(spec, normalized);
        if (serialized != stored.toString())
            m_database->saveSetting(key, serialized);
    }
    bool tooltipCountValid = false;
    const int tooltipSessions
        = storedValues.value(QString::fromLatin1(kPlayerControlTooltipSessionsKey)).toInt(&tooltipCountValid);
    m_playerControlTooltipSessions
        = tooltipCountValid ? qBound(0, tooltipSessions, kPlayerControlTooltipSessionLimit) : 0;

    MpvConfigPolicy configPolicy = validatedPlatformMpvConfigPolicy(m_mpvConfigMode, m_mpvConfigDirectory);
    if (!configPolicy.valid) {
        qWarning() << "settings: invalid persisted mpv configuration policy; disabling custom config";
        m_mpvConfigMode = QStringLiteral("disabled");
        m_mpvConfigDirectory.clear();
        m_values.insert(QStringLiteral("playback/mpvConfigMode"), m_mpvConfigMode);
        m_values.insert(QStringLiteral("playback/mpvConfigDirectory"), m_mpvConfigDirectory);
        m_database->saveSetting(QStringLiteral("playback/mpvConfigMode"), m_mpvConfigMode);
        m_database->saveSetting(QStringLiteral("playback/mpvConfigDirectory"), m_mpvConfigDirectory);
        configPolicy = validatedPlatformMpvConfigPolicy(m_mpvConfigMode, m_mpvConfigDirectory);
    } else if (configPolicy.mode == MpvConfigPolicy::Mode::Custom && configPolicy.directory != m_mpvConfigDirectory) {
        m_mpvConfigDirectory = configPolicy.directory;
        m_values.insert(QStringLiteral("playback/mpvConfigDirectory"), m_mpvConfigDirectory);
        m_database->saveSetting(QStringLiteral("playback/mpvConfigDirectory"), m_mpvConfigDirectory);
    }

    m_localSettingsLoaded = true;
    if (platformUsesPerOutputAudioDelay())
        loadCurrentAudioDelay();

    if (m_player) {
        m_player->setNightModeEnabled(m_nightModeEnabled);
        m_player->setToneMappingVisualizationEnabled(m_toneMappingVisualizationEnabled);
        applyAudioDelayToPlayer();
        m_player->setAudioOutputMode(m_audioOutputMode);
        m_player->setForwardCacheSizeMiB(m_forwardCacheSizeMiB);
        m_player->setMpvConfigPolicy(configPolicy);
    }
    applyArtworkEncoding();
    applyPlaybackPreferences();
    if (m_player)
        m_player->setSubtitlePreferences(m_subtitlePreferences);

    emit settingsValuesChanged();
    emit nightModeChanged();
    emit audioDelayChanged();
    emit subtitleSettingsChanged();
    emit buttonRemapChanged();
    emit appearanceChanged();
    emit remoteControlSettingsChanged();
}

void SettingsController::loadRemote()
{
    if (m_remoteLoadStarted || !m_api || m_api->session().accessToken.isEmpty())
        return;
    m_remoteLoadStarted = true;

    Async::runScoped(
        this, m_api->fetchCultures(),
        [this](const QJsonArray& cultures) {
            QStringList codes { QString() };
            QStringList labels { QStringLiteral("Any language") };
            QSet<QString> seen { QString() };

            for (const QJsonValue& value : cultures) {
                const QJsonObject culture = value.toObject();
                const QString code = culture.value(QStringLiteral("ThreeLetterISOLanguageName")).toString();
                if (code.isEmpty() || seen.contains(code))
                    continue;
                QString label = culture.value(QStringLiteral("DisplayName")).toString();
                if (label.isEmpty())
                    label = code.toUpper();
                seen.insert(code);
                codes.push_back(code);
                labels.push_back(label);
            }

            if (!m_subtitlePreferences.language.isEmpty() && !seen.contains(m_subtitlePreferences.language)) {
                codes.push_back(m_subtitlePreferences.language);
                labels.push_back(m_subtitlePreferences.language.toUpper());
            }

            m_subtitleLanguageCodes = codes;
            m_subtitleLanguageLabels = labels;
            emit subtitleSettingsChanged();
        },
        [](const std::exception_ptr& error) {
            qWarning() << "subtitles: culture list failed" << exceptionMessage(error);
        });

    Async::runScoped(
        this, m_api->fetchUserConfiguration(),
        [this](const QJsonObject& configuration) {
            m_userConfiguration = configuration;
            m_subtitlePreferences.audioLanguage
                = configuration.value(QStringLiteral("AudioLanguagePreference")).toString();
            const SettingSpec& languageSpec = specForKey("subtitles/language");
            const SettingSpec& modeSpec = specForKey("subtitles/mode");
            const SettingSpec& audioModeSpec = specForKey("audio/trackMode");
            setSchemaValue(languageSpec, configuration.value(QStringLiteral("SubtitleLanguagePreference")).toString(),
                true, false, false);
            setSchemaValue(modeSpec,
                configuration.value(QStringLiteral("SubtitleMode")).toString(QStringLiteral("Default")), true, false,
                false);
            setSchemaValue(audioModeSpec,
                configuration.value(QStringLiteral("PlayDefaultAudioTrack")).toBool(true) ? QStringLiteral("Default")
                                                                                          : QStringLiteral("Smart"),
                true, false, false);
            applySubtitlePreferencesToPlayer();
            emit settingsValuesChanged();
            emit subtitleSettingsChanged();
        },
        [](const std::exception_ptr& error) {
            qWarning() << "subtitles: user configuration failed" << exceptionMessage(error);
        });
}

void SettingsController::clearRemote()
{
    m_userConfiguration = {};
    m_remoteLoadStarted = false;
}

void SettingsController::completePlayerControlTooltipSession()
{
    if (m_playerControlTooltipSessions >= kPlayerControlTooltipSessionLimit)
        return;
    ++m_playerControlTooltipSessions;
    m_database->saveSetting(
        QString::fromLatin1(kPlayerControlTooltipSessionsKey), QString::number(m_playerControlTooltipSessions));
    if (m_playerControlTooltipSessions == kPlayerControlTooltipSessionLimit)
        emit playerControlTooltipsEnabledChanged();
}

void SettingsController::setValue(const QString& key, const QVariant& value)
{
    const SettingSpec *spec = findSettingSpec(key);
    if (!spec) {
        qWarning() << "settings: unknown key" << key;
        return;
    }
    if (!spec->persisted) {
        qWarning() << "settings: external row cannot be persisted through SettingsController" << key;
        return;
    }
    if (spec->target == SettingTarget::AudioDelay) {
        setAudioDelayMs(value.toInt());
        return;
    }
    if (spec->target == SettingTarget::MpvConfigMode) {
        const QString mode = normalizedSettingValue(*spec, value).toString();
        const MpvConfigPolicy policy = validatedPlatformMpvConfigPolicy(mode, m_mpvConfigDirectory);
        if (!policy.valid) {
            emit errorOccurred(policy.error);
            return;
        }
        setSchemaValue(*spec, mode, true, true, true);
        return;
    }
    if (spec->target == SettingTarget::MpvConfigDirectory) {
        const MpvConfigPolicy policy = validatedPlatformMpvConfigPolicy(QStringLiteral("custom"), value.toString());
        if (!policy.valid) {
            emit errorOccurred(policy.error);
            return;
        }
        setSchemaValue(*spec, policy.directory, true, true, true);
        return;
    }
    setSchemaValue(*spec, value, true, true, true);
}

void SettingsController::previewValue(const QString& key, const QVariant& value)
{
    const SettingSpec *spec = findSettingSpec(key);
    if (!spec || !m_player)
        return;

    SubtitlePreferences preview = m_subtitlePreferences;
    const QVariant normalized = normalizedSettingValue(*spec, value);
    switch (spec->target) {
    case SettingTarget::SubtitleVerticalPosition:
        preview.verticalPosition = normalized.toInt();
        break;
    case SettingTarget::SubtitleScale:
        preview.scalePercent = normalized.toInt();
        break;
    case SettingTarget::SubtitleBitmapSharpness:
        preview.bitmapSharpnessPercent = normalized.toInt();
        break;
    case SettingTarget::SubtitleBitmapShadowCoreSize:
        preview.bitmapShadowCoreSize = normalized.toInt();
        break;
    case SettingTarget::SubtitleBitmapShadowCoreGrow:
        preview.bitmapShadowCoreGrow = normalized.toInt();
        break;
    case SettingTarget::SubtitleBitmapShadowCoreOpacity:
        preview.bitmapShadowCoreOpacityPercent = normalized.toInt();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadSize:
        preview.bitmapShadowSpreadSize = normalized.toInt();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadGrow:
        preview.bitmapShadowSpreadGrow = normalized.toInt();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadX:
        preview.bitmapShadowSpreadX = normalized.toInt();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadY:
        preview.bitmapShadowSpreadY = normalized.toInt();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadOpacity:
        preview.bitmapShadowSpreadOpacityPercent = normalized.toInt();
        break;
    case SettingTarget::SubtitleHdrBrightness:
        preview.hdrBrightnessPercent = normalized.toInt();
        break;
    default:
        return;
    }
    m_player->previewSubtitlePreferences(preview);
}

void SettingsController::setNightModeEnabled(bool enabled)
{
    setValue(QStringLiteral("settings/nightMode"), enabled);
}
void SettingsController::setAudioDelayMs(int delayMs)
{
    const SettingSpec& spec = specForKey("settings/audioDelayMs");
    if (!platformUsesPerOutputAudioDelay()) {
        setSchemaValue(spec, delayMs, true, true, true);
        return;
    }

    const int normalized = normalizedSettingValue(spec, delayMs).toInt();
    if (m_audioDelayMs == normalized)
        return;

    const int previous = m_audioDelayMs;
    m_audioOutputLoadGeneration.invalidate();
    m_audioDelayMs = normalized;
    m_values.insert(keyString(spec), normalized);
    m_database->saveSetting(
        platformAudioDelayStorageKey(m_currentAudioOutput), serializedSettingValue(spec, normalized));
    applyAudioDelayToPlayer();
    qInfo() << "app: audio delay trim for" << normalizedPlatformAudioRoute(m_currentAudioOutput) << previous << "->"
            << m_audioDelayMs << "ms; automatic" << m_automaticAudioDelayMs << "ms; effective"
            << qBound(-2000, m_automaticAudioDelayMs + m_audioDelayMs, 2000) << "ms";
    emit settingChanged(keyString(spec));
    emit settingsValuesChanged();
    emit audioDelayChanged();
}
void SettingsController::setUiScalePercent(int percent)
{
    setValue(QStringLiteral("appearance/uiScalePercent"), percent);
}
void SettingsController::setSubtitleLanguageIndex(int index)
{
    if (index >= 0 && index < m_subtitleLanguageCodes.size())
        setValue(QStringLiteral("subtitles/language"), m_subtitleLanguageCodes.at(index));
}

void SettingsController::resetSubtitleAppearance()
{
    for (const SettingSpec& spec : settingSpecs()) {
        if (spec.persisted && QLatin1String(spec.group) == QLatin1String("Subtitle Appearance"))
            setValue(keyString(spec), settingDefaultValue(spec));
    }
}
bool SettingsController::setSchemaValue(
    const SettingSpec& spec, const QVariant& value, bool persist, bool apply, bool notify)
{
    const QString key = keyString(spec);
    const QVariant normalized = normalizedSettingValue(spec, value);
    if (m_values.contains(key) && m_values.value(key) == normalized)
        return false;

    const int previousAudioDelayMs = m_audioDelayMs;
    const QString previousAudioOutputMode = m_audioOutputMode;

    m_values.insert(key, normalized);
    applySchemaValue(spec, normalized, apply);

    if (persist)
        m_database->saveSetting(key, serializedSettingValue(spec, normalized));

    if (apply && spec.target == SettingTarget::AudioDelay) {
        qInfo() << "app: audio delay changed" << previousAudioDelayMs << "->" << m_audioDelayMs << "ms";
    } else if (apply && spec.target == SettingTarget::AudioOutput) {
        qInfo() << "app: audio output mode changed" << previousAudioOutputMode << "->" << m_audioOutputMode;
    }

    if (notify) {
        emit settingChanged(key);
        emit settingsValuesChanged();
        emitSchemaSignals(spec);
    }
    return true;
}

void SettingsController::applySchemaValue(const SettingSpec& spec, const QVariant& value, bool apply)
{
    switch (spec.target) {
    case SettingTarget::External:
        break;
    case SettingTarget::NightMode:
        m_nightModeEnabled = value.toBool();
        if (apply && m_player)
            m_player->setNightModeEnabled(m_nightModeEnabled);
        break;
    case SettingTarget::CastButtonEnabled:
        m_castButtonEnabled = value.toBool();
        break;
    case SettingTarget::RemoteControlTargetEnabled:
        m_remoteControlTargetEnabled = value.toBool();
        if (m_api)
            m_api->setRemoteControlTargetEnabled(m_remoteControlTargetEnabled);
        break;
    case SettingTarget::ToneMappingVisualization:
        m_toneMappingVisualizationEnabled = value.toBool();
        if (apply && m_player)
            m_player->setToneMappingVisualizationEnabled(m_toneMappingVisualizationEnabled);
        break;
    case SettingTarget::MaxStreamingHeight:
        m_maxStreamingHeight = value.toInt();
        if (apply)
            applyPlaybackPreferences();
        break;
    case SettingTarget::ManualStreamingBitrate:
        m_manualStreamingBitrate = value.toBool();
        if (apply)
            applyPlaybackPreferences();
        break;
    case SettingTarget::MaxStreamingBitrate:
        m_maxStreamingBitrateMbps = value.toInt();
        if (apply)
            applyPlaybackPreferences();
        break;
    case SettingTarget::UnlimitedLocalBitrate:
        m_unlimitedLocalBitrate = value.toBool();
        if (apply)
            applyPlaybackPreferences();
        break;
    case SettingTarget::PreferRemux:
        m_preferRemux = value.toBool();
        if (apply)
            applyPlaybackPreferences();
        break;
    case SettingTarget::ForwardCacheSize:
        m_forwardCacheSizeMiB = value.toInt();
        if (apply && m_player)
            m_player->setForwardCacheSizeMiB(m_forwardCacheSizeMiB);
        break;
    case SettingTarget::PlayerVolumeSlider:
        break;
    case SettingTarget::AudioDelay:
        m_audioDelayMs = value.toInt();
        if (apply)
            applyAudioDelayToPlayer();
        break;
    case SettingTarget::AudioOutput:
        m_audioOutputMode = value.toString();
        if (apply && m_player)
            m_player->setAudioOutputMode(m_audioOutputMode);
        break;
    case SettingTarget::VideoOutputMode:
        m_videoOutputMode = value.toString();
        if (m_player)
            m_player->setDirectVideoOutput(m_videoOutputMode == QLatin1String("direct"));
        break;
    case SettingTarget::AutoAdjustRenderQuality:
        m_autoAdjustRenderQuality = value.toBool();
        break;
    case SettingTarget::RenderQuality:
        m_renderQuality = value.toString();
        if (m_player)
            m_player->setRenderQuality(MpvOptionProfile::renderQualityFromName(m_renderQuality));
        break;
    case SettingTarget::HardwareDecoding:
        if (m_player)
            m_player->setHardwareDecoding(value.toBool());
        break;
    case SettingTarget::HdrOutputMode:
        if (m_player)
            m_player->setHdrOutputPreference(value.toString());
        // The mpv side of this applies immediately; the swapchain side cannot,
        // because Qt fixes a window's format when the window is created. Keep
        // it somewhere readable before the next window exists.
        RenderTargetPolicy::rememberPreference(RenderTargetPolicy::preferenceFromName(value.toString()));
        break;
    case SettingTarget::GraphicsApi:
        // Qt fixes the scene graph's backend for the life of the process, so
        // like the swapchain format this is recorded for the next launch and
        // read back before QGuiApplication exists.
        RenderTargetPolicy::rememberGraphicsApi(RenderTargetPolicy::graphicsApiFromName(value.toString()));
        break;
    case SettingTarget::HdrPeakBrightness:
        if (m_player)
            m_player->setHdrPeakNits(value.toInt());
        break;
    case SettingTarget::UiScale:
        m_uiScalePercent = value.toInt();
        break;
    case SettingTarget::AutomaticUpdates:
        // Announced even on the initial load, which is what starts the first
        // check: nothing checks for updates until settings have said it may.
        emit automaticUpdatesChanged(value.toBool());
        break;
    case SettingTarget::ArtworkFormat:
        m_artworkFormat = value.toString();
        if (apply)
            applyArtworkEncoding();
        break;
    case SettingTarget::ArtworkWebpQuality:
        m_artworkWebpQuality = value.toInt();
        if (apply)
            applyArtworkEncoding();
        break;
    case SettingTarget::ArtworkJpegQuality:
        m_artworkJpegQuality = value.toInt();
        if (apply)
            applyArtworkEncoding();
        break;
    case SettingTarget::AudioTrackMode:
        m_subtitlePreferences.audioMode = value.toString();
        if (apply) {
            saveSubtitleUserConfiguration();
            applySubtitlePreferencesToPlayer();
        }
        break;
    case SettingTarget::RememberSeriesAudioTrack:
        break;
    case SettingTarget::SubtitleLanguage:
        m_subtitlePreferences.language = value.toString();
        if (apply) {
            saveSubtitleUserConfiguration();
            applySubtitlePreferencesToPlayer();
        }
        break;
    case SettingTarget::SubtitleMode:
        m_subtitlePreferences.mode = value.toString();
        if (apply) {
            saveSubtitleUserConfiguration();
            applySubtitlePreferencesToPlayer();
        }
        break;
    case SettingTarget::SubtitleStyling:
        m_subtitlePreferences.styling = value.toString();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleTextWeight:
        m_subtitlePreferences.textWeight = value.toString();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleFont:
        m_subtitlePreferences.font = value.toString();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleTextColor:
        m_subtitlePreferences.textColor = value.toString();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleTextColorOverride:
        m_subtitlePreferences.overrideTextColor = value.toBool();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleDropShadow:
        m_subtitlePreferences.dropShadow = value.toString();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleTextBackground:
        m_subtitlePreferences.textBackground = value.toString();
        break;
    case SettingTarget::SubtitleVerticalPosition:
        m_subtitlePreferences.verticalPosition = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleScale:
        m_subtitlePreferences.scalePercent = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitlePositionAndSizeOverride:
        m_subtitlePreferences.alwaysOverridePositionAndSize = value.toBool();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapSharpness:
        m_subtitlePreferences.bitmapSharpnessPercent = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleRecolorImages:
        m_subtitlePreferences.recolorImageSubtitles = value.toBool();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowEnabled:
        m_subtitlePreferences.bitmapShadowEnabled = value.toBool();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowCoreSize:
        m_subtitlePreferences.bitmapShadowCoreSize = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowCoreGrow:
        m_subtitlePreferences.bitmapShadowCoreGrow = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowCoreOpacity:
        m_subtitlePreferences.bitmapShadowCoreOpacityPercent = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadEnabled:
        m_subtitlePreferences.bitmapShadowSpreadEnabled = value.toBool();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadSize:
        m_subtitlePreferences.bitmapShadowSpreadSize = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadGrow:
        m_subtitlePreferences.bitmapShadowSpreadGrow = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadX:
        m_subtitlePreferences.bitmapShadowSpreadX = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadY:
        m_subtitlePreferences.bitmapShadowSpreadY = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadOpacity:
        m_subtitlePreferences.bitmapShadowSpreadOpacityPercent = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowDither:
        m_subtitlePreferences.bitmapShadowDither = value.toBool();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleAllowInBlackBars:
        m_subtitlePreferences.allowSubtitlesInBlackBars = value.toBool();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleHdrBrightness:
        m_subtitlePreferences.hdrBrightnessPercent = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::RedButton:
        m_redButtonAction = value.toString();
        break;
    case SettingTarget::GreenButton:
        m_greenButtonAction = value.toString();
        break;
    case SettingTarget::YellowButton:
        m_yellowButtonAction = value.toString();
        break;
    case SettingTarget::BlueButton:
        m_blueButtonAction = value.toString();
        break;
    case SettingTarget::MpvConfigMode:
        m_mpvConfigMode = value.toString();
        if (apply)
            applyMpvConfigPolicy();
        break;
    case SettingTarget::MpvConfigDirectory:
        m_mpvConfigDirectory = value.toString();
        if (apply)
            applyMpvConfigPolicy();
        break;
    }
}

void SettingsController::emitSchemaSignals(const SettingSpec& spec)
{
    switch (spec.target) {
    case SettingTarget::External:
        break;
    case SettingTarget::NightMode:
        emit nightModeChanged();
        break;
    case SettingTarget::CastButtonEnabled:
    case SettingTarget::RemoteControlTargetEnabled:
        emit remoteControlSettingsChanged();
        break;
    case SettingTarget::ToneMappingVisualization:
    case SettingTarget::MaxStreamingHeight:
    case SettingTarget::ManualStreamingBitrate:
    case SettingTarget::MaxStreamingBitrate:
    case SettingTarget::UnlimitedLocalBitrate:
    case SettingTarget::PreferRemux:
    case SettingTarget::ForwardCacheSize:
    case SettingTarget::PlayerVolumeSlider:
    case SettingTarget::AudioOutput:
    case SettingTarget::HardwareDecoding:
        break;
    case SettingTarget::AudioDelay:
        emit audioDelayChanged();
        break;
    case SettingTarget::UiScale:
        emit appearanceChanged();
        break;
    case SettingTarget::ArtworkFormat:
    case SettingTarget::ArtworkWebpQuality:
    case SettingTarget::ArtworkJpegQuality:
        break;
    case SettingTarget::AudioTrackMode:
    case SettingTarget::RememberSeriesAudioTrack:
        break;
    case SettingTarget::SubtitleLanguage:
    case SettingTarget::SubtitleMode:
    case SettingTarget::SubtitleStyling:
    case SettingTarget::SubtitleTextWeight:
    case SettingTarget::SubtitleFont:
    case SettingTarget::SubtitleTextColor:
    case SettingTarget::SubtitleTextColorOverride:
    case SettingTarget::SubtitleDropShadow:
    case SettingTarget::SubtitleTextBackground:
    case SettingTarget::SubtitleVerticalPosition:
    case SettingTarget::SubtitleScale:
    case SettingTarget::SubtitlePositionAndSizeOverride:
    case SettingTarget::SubtitleBitmapSharpness:
    case SettingTarget::SubtitleRecolorImages:
    case SettingTarget::SubtitleBitmapShadowEnabled:
    case SettingTarget::SubtitleBitmapShadowCoreSize:
    case SettingTarget::SubtitleBitmapShadowCoreGrow:
    case SettingTarget::SubtitleBitmapShadowCoreOpacity:
    case SettingTarget::SubtitleBitmapShadowSpreadEnabled:
    case SettingTarget::SubtitleBitmapShadowSpreadSize:
    case SettingTarget::SubtitleBitmapShadowSpreadGrow:
    case SettingTarget::SubtitleBitmapShadowSpreadX:
    case SettingTarget::SubtitleBitmapShadowSpreadY:
    case SettingTarget::SubtitleBitmapShadowSpreadOpacity:
    case SettingTarget::SubtitleBitmapShadowDither:
    case SettingTarget::SubtitleAllowInBlackBars:
    case SettingTarget::SubtitleHdrBrightness:
        emit subtitleSettingsChanged();
        break;
    case SettingTarget::RedButton:
    case SettingTarget::GreenButton:
    case SettingTarget::YellowButton:
    case SettingTarget::BlueButton:
        emit buttonRemapChanged();
        break;
    case SettingTarget::MpvConfigMode:
    case SettingTarget::MpvConfigDirectory:
        break;
    }
}

void SettingsController::applyArtworkEncoding()
{
    if (!m_artwork)
        return;
    // "auto" is resolved here rather than in the schema so the stored value
    // stays portable: the same profile restored on a TV and on a desktop asks
    // each for the format that suits it.
    const QString format = m_artworkFormat == QLatin1String("auto")
        ? QString::fromLatin1(platformDefaultArtworkFormat())
        : m_artworkFormat;
    m_artwork->setArtworkEncoding(format, m_artworkWebpQuality, m_artworkJpegQuality);
}

void SettingsController::applyPlaybackPreferences()
{
    if (!m_api)
        return;
    const qint64 manualBitrate
        = m_manualStreamingBitrate ? static_cast<qint64>(m_maxStreamingBitrateMbps) * 1'000'000 : 0;
    m_api->setPlaybackPreferences(manualBitrate, m_unlimitedLocalBitrate, m_preferRemux, m_maxStreamingHeight);
}

void SettingsController::applyAudioDelayToPlayer()
{
    if (!m_player)
        return;
    m_player->setAudioDelayMs(qBound(-2000, m_automaticAudioDelayMs + m_audioDelayMs, 2000));
}

void SettingsController::updateAudioOutputRoute(const QString& output, int displayLatencyMs, int outputLatencyMs)
{
    const QString normalizedOutput = normalizedPlatformAudioRoute(output);
    const int automaticDelay = platformAutomaticAudioDelayMs(normalizedOutput, displayLatencyMs, outputLatencyMs);
    const bool outputChanged = normalizedOutput != m_currentAudioOutput;
    const bool automaticDelayChanged = automaticDelay != m_automaticAudioDelayMs;
    const bool displayLatencyChanged = displayLatencyMs != m_displayLatencyMs;
    if (!outputChanged && !automaticDelayChanged && !displayLatencyChanged)
        return;

    qInfo() << "app: audio output" << normalizedOutput << "display latency" << displayLatencyMs << "ms; output latency"
            << outputLatencyMs << "ms; automatic delay" << automaticDelay << "ms";

    m_audioOutputLoadGeneration.invalidate();
    m_currentAudioOutput = normalizedOutput;
    m_automaticAudioDelayMs = automaticDelay;
    m_displayLatencyMs = displayLatencyMs;
    if (outputChanged) {
        m_audioDelayMs = 0;
        m_values.insert(QStringLiteral("settings/audioDelayMs"), m_audioDelayMs);
    }
    applyAudioDelayToPlayer();

    emit audioOutputDeviceChanged();
    if (outputChanged) {
        emit settingChanged(QStringLiteral("settings/audioDelayMs"));
        emit settingsValuesChanged();
        emit audioDelayChanged();
        if (m_localSettingsLoaded && platformUsesPerOutputAudioDelay())
            loadCurrentAudioDelay();
    }
}

void SettingsController::loadCurrentAudioDelay()
{
    if (!platformUsesPerOutputAudioDelay())
        return;
    const QString output = normalizedPlatformAudioRoute(m_currentAudioOutput);
    const auto token = m_audioOutputLoadGeneration.next();
    Async::runLatest(
        this, m_database->loadSettingAsync(platformAudioDelayStorageKey(output), QStringLiteral("0")),
        m_audioOutputLoadGeneration, token,
        [this, output](const QString& stored) {
            const SettingSpec& spec = specForKey("settings/audioDelayMs");
            applyLoadedAudioDelay(output, normalizedSettingValue(spec, stored).toInt());
        },
        [](const std::exception_ptr& error) {
            qWarning() << "app: failed to load per-output audio delay trim:" << exceptionMessage(error);
        },
        "load per-output audio delay trim");
}

void SettingsController::applyLoadedAudioDelay(const QString& output, int delayMs)
{
    if (normalizedPlatformAudioRoute(output) != m_currentAudioOutput)
        return;

    m_audioDelayMs = delayMs;
    m_values.insert(QStringLiteral("settings/audioDelayMs"), delayMs);
    applyAudioDelayToPlayer();
    qInfo() << "app: loaded audio delay trim for" << m_currentAudioOutput << delayMs << "ms; automatic"
            << m_automaticAudioDelayMs << "ms; effective"
            << qBound(-2000, m_automaticAudioDelayMs + m_audioDelayMs, 2000) << "ms";
    emit settingChanged(QStringLiteral("settings/audioDelayMs"));
    emit settingsValuesChanged();
    emit audioDelayChanged();
}

void SettingsController::saveSubtitleUserConfiguration()
{
    if (!m_api || m_api->session().accessToken.isEmpty())
        return;

    QJsonObject configuration = m_userConfiguration;
    configuration.insert(QStringLiteral("SubtitleLanguagePreference"), m_subtitlePreferences.language);
    configuration.insert(QStringLiteral("SubtitleMode"), m_subtitlePreferences.mode);
    const bool smartAudio = m_subtitlePreferences.audioMode == QStringLiteral("Smart");
    configuration.insert(QStringLiteral("PlayDefaultAudioTrack"), !smartAudio);
    m_userConfiguration = configuration;

    Async::runScoped(
        this, m_api->updateUserConfiguration(configuration), []() {},
        [this](const std::exception_ptr& error) { emit errorOccurred(exceptionMessage(error)); });
}

void SettingsController::applyMpvConfigPolicy()
{
    const MpvConfigPolicy policy = validatedPlatformMpvConfigPolicy(m_mpvConfigMode, m_mpvConfigDirectory);
    if (policy.valid && m_player)
        m_player->setMpvConfigPolicy(policy);
}

void SettingsController::applySubtitlePreferencesToPlayer()
{
    if (m_player)
        m_player->setSubtitlePreferences(m_subtitlePreferences);
}

} // namespace JellyfinNative
