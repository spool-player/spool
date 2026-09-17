#pragma once

#include <QVariant>
#include <QVariantList>
#include <QVector>

namespace JellyfinNative {

enum class SettingType {
    Action,
    ReadOnly,
    Text,
    Toggle,
    Select,
    Slider,
};

enum class SettingLevel {
    Essential,
    Advanced,
    Expert,
};

enum class SettingPlatform {
    All,
    Desktop,
    WebOS,
    Android,
};

enum class SettingTarget {
    External,
    NightMode,
    CastButtonEnabled,
    RemoteControlTargetEnabled,
    ToneMappingVisualization,
    MaxStreamingHeight,
    ManualStreamingBitrate,
    MaxStreamingBitrate,
    UnlimitedLocalBitrate,
    PreferRemux,
    ForwardCacheSize,
    PlayerVolumeSlider,
    AudioDelay,
    AudioOutput,
    UiScale,
    ArtworkFormat,
    ArtworkWebpQuality,
    ArtworkJpegQuality,
    AudioTrackMode,
    RememberSeriesAudioTrack,
    SubtitleLanguage,
    SubtitleMode,
    SubtitleStyling,
    SubtitleTextWeight,
    SubtitleFont,
    SubtitleTextColor,
    SubtitleTextColorOverride,
    SubtitleDropShadow,
    SubtitleTextBackground,
    SubtitleVerticalPosition,
    SubtitleScale,
    SubtitlePositionAndSizeOverride,
    SubtitleBitmapSharpness,
    SubtitleRecolorImages,
    SubtitleBitmapShadowEnabled,
    SubtitleBitmapShadowCoreSize,
    SubtitleBitmapShadowCoreGrow,
    SubtitleBitmapShadowCoreOpacity,
    SubtitleBitmapShadowSpreadEnabled,
    SubtitleBitmapShadowSpreadSize,
    SubtitleBitmapShadowSpreadGrow,
    SubtitleBitmapShadowSpreadX,
    SubtitleBitmapShadowSpreadY,
    SubtitleBitmapShadowSpreadOpacity,
    SubtitleBitmapShadowDither,
    SubtitleAllowInBlackBars,
    SubtitleHdrBrightness,
    RedButton,
    GreenButton,
    YellowButton,
    BlueButton,
    MpvConfigMode,
    MpvConfigDirectory,
    AutomaticUpdates,
    RenderQuality,
    AutoAdjustRenderQuality,
    HardwareDecoding,
    HdrOutputMode,
    GraphicsApi,
    HdrPeakBrightness,
    VideoOutputMode,
};

enum class SettingNormalizer {
    Bool,
    IntRange,
    PowerOfTwoRange,
    String,
    // Snaps to the spec's choices, or passes the value through when the choices
    // are filled in at runtime rather than declared here.
    Choice,
    AudioOutput,

    SubtitleFont,
    SubtitleColor,
};

struct SettingChoice {
    const char *value;
    const char *label;
};

struct SettingSpec {
    const char *key;
    const char *group;
    const char *title;
    const char *description;
    SettingType type;
    const char *defaultValue;
    SettingTarget target;
    SettingNormalizer normalizer;
    const SettingChoice *choices = nullptr;
    qsizetype choiceCount = 0;
    int minimum = 0;
    int maximum = 0;
    int step = 1;
    const char *unit = "";
    SettingLevel level = SettingLevel::Essential;
    SettingPlatform platform = SettingPlatform::All;
    const char *dependsOnKey = "";
    const char *dependsOnValue = "";
    bool persisted = true;
    bool requiresHdrPlayback = false;

    // Declaration modifiers. Each returns a copy so specs read as one
    // expression: slider(...).advanced().onDesktop().
    SettingSpec advanced() const;
    SettingSpec expert() const;
    SettingSpec onDesktop() const;
    SettingSpec onWebOS() const;
    SettingSpec onAndroid() const;
    SettingSpec whenSetTo(const char *otherKey, const char *otherValue) const;
    SettingSpec duringHdrPlayback() const;
};

const QVector<SettingSpec>& settingSpecs();
const SettingSpec *findSettingSpec(const QString& key);
QVariant settingDefaultValue(const SettingSpec& spec);
QVariant normalizedSettingValue(const SettingSpec& spec, const QVariant& value);
QString serializedSettingValue(const SettingSpec& spec, const QVariant& value);
QVariantList settingSchemaModel();

} // namespace JellyfinNative
