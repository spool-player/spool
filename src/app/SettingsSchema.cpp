#include "SettingsSchema.h"

#include "../media/MediaTypes.h"
#include "../platform/PlatformSettingsPolicy.h"

#include <QVariantMap>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace Spool {
namespace {

    // Enhanced puts every frame through libplacebo, which is where the
    // scaling, tone mapping and debanding live. Direct hands the decoder its
    // own surface and lets the display pipeline do that work instead: far
    // cheaper, and the only thing a weak television box can sustain at 4K.
    constexpr SettingChoice kVideoOutputChoices[] = {
        { "enhanced", "Enhanced" },
        { "direct", "Direct" },
    };
    // Named for what is traded, not for a number: each rung down gives up
    // scaler taps, linear-light resampling and measured tone curves, in that
    // order.
    constexpr SettingChoice kRenderQualityChoices[] = {
        { "maximum", "Maximum" },
        { "high", "High" },
        { "balanced", "Balanced" },
        { "fast", "Fast" },
    };
    constexpr SettingChoice kSoftwareRendererChoices[] = {
        { "auto", "Automatic" },
        { "gpu", "OpenGL (gpu)" },
        { "gpu-next", "OpenGL (gpu-next, experimental)" },
    };
    constexpr SettingChoice kHdrOutputChoices[] = {
        { "auto", "Automatic" },
        { "always", "Always" },
        { "never", "Never" },
    };
    // Only the backends this platform's build can actually create. The scene
    // graph runs on one of these, and it decides what the swapchain can
    // present: OpenGL has no HDR format to offer on any of them, so choosing it
    // is choosing SDR.
    constexpr SettingChoice kGraphicsApiChoices[] = {
        { "auto", "Automatic" },
#if defined(Q_OS_WIN)
        { "d3d11", "Direct3D 11" },
#endif
    // Asked the way MpvVideoItem.cpp asks it, and for the same reason:
    // QT_CONFIG reads a feature macro this translation unit has no answer
    // for, and Qt owning QVulkanInstance says nothing about whether the
    // Vulkan headers the player's own path needs are here.
#if __has_include(<QVulkanInstance>) && __has_include(<vulkan/vulkan.h>)
        { "vulkan", "Vulkan" },
#endif
        { "opengl", "OpenGL" },
    };
    constexpr SettingChoice kAccentChoices[] = { { "0", "Blue" }, { "1", "Purple" }, { "2", "Indigo" } };
    constexpr SettingChoice kRailLabelChoices[]
        = { { "Never", "Never" }, { "On focus", "On focus" }, { "Always", "Always" } };
    constexpr SettingChoice kTextRenderModeChoices[] = { { "0", "Standard" }, { "1", "Curve" } };
    constexpr SettingChoice kProviderUpdateChoices[]
        = { { "auto", "Automatic" }, { "ask", "Ask first" }, { "manual", "Only when I check" } };
    constexpr SettingChoice kArtworkFormatChoices[]
        = { { "auto", "Automatic" }, { "webp", "WebP (smaller downloads)" }, { "jpeg", "JPEG (faster to decode)" } };
    constexpr SettingChoice kTechnicalMetadataChoices[]
        = { { "Always", "Always" }, { "On details only", "Details page only" }, { "Hidden", "Never" } };
    // "%1" is replaced with the user's preferred-language name when shown.
    constexpr SettingChoice kAudioTrackModeChoices[]
        = { { "Default", "Default (the file's own default)" }, { "Smart", "Smart (%1 when available)" } };
    constexpr SettingChoice kSubtitleModeChoices[] = { { "Default", "Default (whatever the file marks)" },
        { "Smart", "Smart (%1 when the audio is another language)" }, { "OnlyForced", "Only forced" },
        { "Always", "Always (%1 when available)" }, { "None", "Off" } };
    // Resolution is offered on its own so it can be answered on its own: the
    // bitrate ceiling below caps how many bits arrive, this caps how large a
    // picture the server may send, and neither implies the other.
    constexpr SettingChoice kMaxStreamingHeightChoices[] = {
        { "0", "Source resolution" },
        { "2160", "4K" },
        { "1080", "1080p" },
        { "720", "720p" },
        { "480", "480p" },
        { "360", "360p" },
    };
    constexpr SettingChoice kMpvConfigModeChoices[]
        = { { "disabled", "Off" }, { "standard", "Standard mpv directory" }, { "custom", "Custom directory" } };
    constexpr SettingChoice kSubtitleStylingChoices[] = { { "Auto", "Automatic" },
        { "Native", "Keep the subtitle's own style" }, { "Custom", "Always use my style" } };
    constexpr SettingChoice kSubtitleTextWeightChoices[] = { { "normal", "Normal" }, { "bold", "Bold" } };
    constexpr SettingChoice kSubtitleFontChoices[]
        = { { "", "Atkinson Hyperlegible" }, { "interface", "IBM Plex Sans" } };
    constexpr SettingChoice kSubtitleTextColorChoices[] = { { "#ffffff", "White" }, { "#d3d3d3", "Light gray" },
        { "#808080", "Gray" }, { "#ffff00", "Yellow" }, { "#008000", "Green" }, { "#00ffff", "Cyan" },
        { "#0000ff", "Blue" }, { "#ff00ff", "Magenta" }, { "#ff0000", "Red" }, { "#000000", "Black" } };
    constexpr SettingChoice kSubtitleDropShadowChoices[] = { { "none", "None" }, { "raised", "Raised" },
        { "depressed", "Depressed" }, { "uniform", "Uniform" }, { "", "Drop shadow" } };
    constexpr SettingChoice kSubtitleBackgroundChoices[]
        = { { "transparent", "None" }, { "translucent", "Translucent black" }, { "opaque", "Solid black" } };

    constexpr SettingChoice kButtonActionChoices[] = { { "none", "No action" }, { "togglePause", "Play / Pause" },
        { "toggleSubs", "Toggle subtitles" }, { "cycleSubs", "Cycle subtitles" }, { "cycleAudio", "Cycle audio track" },
        { "queuePrevious", "Previous queue item" }, { "queueNext", "Next queue item" },
        { "skipBack10", "Skip back 10 s" }, { "skipForward10", "Skip forward 10 s" },
        { "skipBack30", "Skip back 30 s" }, { "skipForward30", "Skip forward 30 s" },
        { "skipBack90", "Skip back 90 s" }, { "skipForward90", "Skip forward 90 s" },
        { "skipBackAndEnableSubs", "Skip back 10 s + enable subs" }, { "skipSegment", "Skip intro / outro" },
        { "showInfo", "Show info" }, { "stop", "Stop playback" } };

    SettingSpec toggleSpec(const char *key, const char *group, const char *title, const char *description,
        bool defaultOn, SettingTarget target)
    {
        return { key, group, title, description, SettingType::Toggle, defaultOn ? "true" : "false", target,
            SettingNormalizer::Bool };
    }

    SettingSpec selectSpec(const char *key, const char *group, const char *title, const char *description,
        const char *defaultValue, const SettingChoice *choices, qsizetype choiceCount, SettingTarget target,
        SettingNormalizer normalizer = SettingNormalizer::Choice)
    {
        SettingSpec spec { key, group, title, description, SettingType::Select, defaultValue, target, normalizer };
        spec.choices = choices;
        spec.choiceCount = choiceCount;
        return spec;
    }

    template <size_t N>
    SettingSpec selectSpec(const char *key, const char *group, const char *title, const char *description,
        const char *defaultValue, const SettingChoice (&choices)[N], SettingTarget target,
        SettingNormalizer normalizer = SettingNormalizer::Choice)
    {
        return selectSpec(
            key, group, title, description, defaultValue, choices, static_cast<qsizetype>(N), target, normalizer);
    }

    SettingSpec sliderSpec(const char *key, const char *group, const char *title, const char *description,
        const char *defaultValue, int minimum, int maximum, int step, const char *unit, SettingTarget target,
        SettingNormalizer normalizer = SettingNormalizer::IntRange)
    {
        SettingSpec spec { key, group, title, description, SettingType::Slider, defaultValue, target, normalizer };
        spec.minimum = minimum;
        spec.maximum = maximum;
        spec.step = step;
        spec.unit = unit;
        return spec;
    }

    SettingSpec textSpec(
        const char *key, const char *group, const char *title, const char *description, SettingTarget target)
    {
        return { key, group, title, description, SettingType::Text, "", target, SettingNormalizer::String };
    }

    // Rows the settings page owns end to end: the schema only describes how to
    // draw them, and SettingsController never stores their value.
    SettingSpec pageSpec(const char *key, const char *group, const char *title, const char *description,
        SettingType type, const SettingChoice *choices, qsizetype choiceCount)
    {
        SettingSpec spec { key, group, title, description, type, "", SettingTarget::External,
            type == SettingType::Select ? SettingNormalizer::Choice : SettingNormalizer::String };
        spec.choices = choices;
        spec.choiceCount = choiceCount;
        spec.persisted = false;
        return spec;
    }

    SettingSpec pageSpec(
        const char *key, const char *group, const char *title, const char *description, SettingType type)
    {
        return pageSpec(key, group, title, description, type, nullptr, 0);
    }

    template <size_t N>
    SettingSpec pageSpec(const char *key, const char *group, const char *title, const char *description,
        SettingType type, const SettingChoice (&choices)[N])
    {
        return pageSpec(key, group, title, description, type, choices, static_cast<qsizetype>(N));
    }

    QString typeName(SettingType type)
    {
        switch (type) {
        case SettingType::Action:
            return QStringLiteral("action");
        case SettingType::ReadOnly:
            return QStringLiteral("readonly");
        case SettingType::Text:
            return QStringLiteral("text");
        case SettingType::Toggle:
            return QStringLiteral("toggle");
        case SettingType::Select:
            return QStringLiteral("select");
        case SettingType::Slider:
            return QStringLiteral("slider");
        }
        return QStringLiteral("readonly");
    }

    QString platformName(SettingPlatform platform)
    {
        switch (platform) {
        case SettingPlatform::All:
            return QStringLiteral("all");
        case SettingPlatform::Desktop:
            return QStringLiteral("desktop");
        case SettingPlatform::WebOS:
            return QStringLiteral("webos");
        case SettingPlatform::Android:
            return QStringLiteral("android");
        }
        return QStringLiteral("all");
    }

    bool boolValue(const QVariant& value)
    {
        if (value.typeId() == QMetaType::Bool)
            return value.toBool();
        const QString text = value.toString().trimmed().toLower();
        return text == QStringLiteral("true") || text == QStringLiteral("1") || text == QStringLiteral("yes");
    }

    // Choices left empty here are filled in at runtime (server languages, for
    // example), so there is nothing to snap an incoming value to.
    QString choiceOrDefault(const SettingSpec& spec, const QString& value)
    {
        if (spec.choiceCount == 0)
            return value;
        for (qsizetype i = 0; i < spec.choiceCount; ++i) {
            if (value == QLatin1String(spec.choices[i].value))
                return value;
        }
        return QLatin1String(spec.defaultValue);
    }

    QString normalizedSubtitleColor(QString color)
    {
        color = color.trimmed().toLower();
        if (!color.startsWith(QLatin1Char('#')) || color.size() != 7)
            return QStringLiteral("#ffffff");
        for (int i = 1; i < color.size(); ++i) {
            const QChar ch = color.at(i);
            if (!ch.isDigit() && (ch < QLatin1Char('a') || ch > QLatin1Char('f')))
                return QStringLiteral("#ffffff");
        }
        return color;
    }

} // namespace

SettingSpec SettingSpec::advanced() const
{
    SettingSpec spec = *this;
    spec.level = SettingLevel::Advanced;
    return spec;
}

SettingSpec SettingSpec::expert() const
{
    SettingSpec spec = *this;
    spec.level = SettingLevel::Expert;
    return spec;
}

SettingSpec SettingSpec::onDesktop() const
{
    SettingSpec spec = *this;
    spec.platform = SettingPlatform::Desktop;
    return spec;
}

SettingSpec SettingSpec::onWebOS() const
{
    SettingSpec spec = *this;
    spec.platform = SettingPlatform::WebOS;
    return spec;
}

SettingSpec SettingSpec::onAndroid() const
{
    SettingSpec spec = *this;
    spec.platform = SettingPlatform::Android;
    return spec;
}

SettingSpec SettingSpec::whenSetTo(const char *otherKey, const char *otherValue) const
{
    SettingSpec spec = *this;
    spec.dependsOnKey = otherKey;
    spec.dependsOnValue = otherValue;
    return spec;
}

SettingSpec SettingSpec::duringHdrPlayback() const
{
    SettingSpec spec = *this;
    spec.requiresHdrPlayback = true;
    return spec;
}

// Declaration order is display order: rows appear in the settings page exactly
// as listed, grouped by the group name they carry.
const QVector<SettingSpec>& settingSpecs()
{
    const PlatformAudioOutputPolicy& audioOutput = platformAudioOutputPolicy();
    static const QVector<SettingSpec> specs {

        sliderSpec("appearance/uiScalePercent", "Appearance", "Interface scale", "", "100", 50, 180, 5, "%",
            SettingTarget::UiScale),
        pageSpec("theme/accent", "Appearance", "Accent colour", "", SettingType::Select, kAccentChoices),
        pageSpec("theme/reducedMotion", "Appearance", "Reduced motion", "", SettingType::Toggle),
        pageSpec(
            "i18n/locale", "Appearance", "Language", "Some text only changes after a restart", SettingType::Select),
        pageSpec("theme/technicalMetadata", "Appearance", "Technical details", "Codec, resolution, and audio format",
            SettingType::Select, kTechnicalMetadataChoices)
            .advanced(),
        pageSpec("theme/railLabels", "Appearance", "Navigation labels", "", SettingType::Select, kRailLabelChoices)
            .advanced(),
        pageSpec("theme/antialiasedText", "Appearance", "Smooth text", "", SettingType::Toggle).advanced(),
        pageSpec("theme/renderMode", "Appearance", "Text rendering", "", SettingType::Select, kTextRenderModeChoices)
            .advanced(),
        selectSpec("artwork/format", "Appearance", "Artwork format",
            "Automatic picks JPEG on TVs, where decoding costs more than downloading", "auto", kArtworkFormatChoices,
            SettingTarget::ArtworkFormat)
            .advanced(),
        sliderSpec("artwork/webpQuality", "Appearance", "WebP quality", "", "75", 40, 100, 1, "",
            SettingTarget::ArtworkWebpQuality)
            .advanced(),
        sliderSpec("artwork/jpegQuality", "Appearance", "JPEG quality",
            "JPEG needs a higher number than WebP to look the same", "82", 40, 100, 1, "",
            SettingTarget::ArtworkJpegQuality)
            .advanced(),
        toggleSpec("remote/acceptCommands", "Remote Control", "Allow remote control", "", true,
            SettingTarget::RemoteControlTargetEnabled),
        selectSpec("audio/trackMode", "Playback", "Audio track", "", "Default", kAudioTrackModeChoices,
            SettingTarget::AudioTrackMode),
        toggleSpec("playback/rememberSeriesAudioTrack", "Playback", "Remember audio track per series", "", true,
            SettingTarget::RememberSeriesAudioTrack),
        toggleSpec("settings/nightMode", "Playback", "Night mode", "Lifts quiet dialogue and tames loud scenes", false,
            SettingTarget::NightMode),
        sliderSpec("settings/audioDelayMs", "Playback", "Audio sync", "", "0", -2000, 2000, 10, "ms",
            SettingTarget::AudioDelay),
        toggleSpec("playback/showVolumeSlider", "Playback", "Volume slider in the player", "", true,
            SettingTarget::PlayerVolumeSlider)
            .onDesktop(),
        sliderSpec("playback/controlFadeDelaySeconds", "Playback", "Hide player controls after", "", "4", 1, 10, 1, "s",
            SettingTarget::External),
        selectSpec("playback/videoOutput", "Playback", "Video output",
            "Enhanced processes each frame on the GPU. Direct sends it straight to the display", "enhanced",
            kVideoOutputChoices, SettingTarget::VideoOutputMode)
            .onAndroid()
            .advanced(),
        selectSpec("playback/softwareRenderer", "Playback", "Software video renderer",
            "For codecs Starfish cannot decode. Automatic uses gpu; gpu-next is experimental. "
            "Keeps lightweight rendering settings. Applies to the next playback",
            "auto", kSoftwareRendererChoices, SettingTarget::SoftwareRenderer)
            .onWebOS()
            .advanced(),
        toggleSpec("playback/hardwareDecoding", "Playback", "Hardware decoding",
            "Use the GPU to decode video when supported. Turn off to use the CPU. Applies to the next playback", true,
            SettingTarget::HardwareDecoding)
            .onDesktop(),
        selectSpec("playback/renderQuality", "Playback", "Picture quality",
            "How much work the GPU does on each frame. Lowered automatically if playback drops frames", "balanced",
            kRenderQualityChoices, SettingTarget::RenderQuality)
            .advanced(),
        toggleSpec("playback/autoAdjustQuality", "Playback", "Adjust quality automatically",
            "Step down a rung when playback drops frames on this device", true, SettingTarget::AutoAdjustRenderQuality)
            .advanced(),
        selectSpec("playback/graphicsApi", "Playback", "Graphics backend",
            "Applies when Spool next starts. Automatic picks the backend this platform presents HDR through. "
            "OpenGL is the SDR compatibility choice",
            "auto", kGraphicsApiChoices, SettingTarget::GraphicsApi)
            .onDesktop()
            .advanced(),
        selectSpec("playback/hdrOutput", "Playback", "HDR output",
            "Applies when Spool next starts. Automatic enables HDR on supported Linux Wayland Vulkan and Windows "
            "Direct3D 11 displays with OS HDR enabled. Unsupported outputs stay SDR",
            "auto", kHdrOutputChoices, SettingTarget::HdrOutputMode)
            .onDesktop()
            .expert(),
        sliderSpec("playback/hdrPeakNits", "Playback", "Display peak brightness",
            "What the display can actually reach. Zero uses OS or compositor-reported luminance when available", "0", 0,
            4000, 50, " nits", SettingTarget::HdrPeakBrightness)
            .onDesktop()
            .expert(),
        selectSpec("settings/audioOutputMode", "Playback", "Audio output", "Applies the next time something plays",
            audioOutput.defaultValue, audioOutput.choices, audioOutput.choiceCount, SettingTarget::AudioOutput,
            SettingNormalizer::AudioOutput)
            .advanced(),
        selectSpec("playback/mpvConfigMode", "Playback", "mpv configuration",
            "Your own mpv config can change or break playback", "disabled", kMpvConfigModeChoices,
            SettingTarget::MpvConfigMode)
            .expert()
            .onDesktop(),
        textSpec("playback/mpvConfigDirectory", "Playback", "mpv directory",
            "Must hold mpv.conf and any scripts you want", SettingTarget::MpvConfigDirectory)
            .expert()
            .onDesktop()
            .whenSetTo("playback/mpvConfigMode", "custom"),

        selectSpec("playback/maxStreamingHeight", "Streaming", "Resolution limit",
            "Anything larger is scaled down by the server", "0", kMaxStreamingHeightChoices,
            SettingTarget::MaxStreamingHeight),
        toggleSpec("playback/manualStreamingBitrate", "Streaming", "Set my own bitrate limit",
            "Otherwise the limit is measured for you", false, SettingTarget::ManualStreamingBitrate),
        pageSpec("action/connectionSpeed", "Streaming", "Connection speed", "", SettingType::Action),
        sliderSpec("playback/maxStreamingBitrateMbps", "Streaming", "Bitrate limit",
            "Anything higher is transcoded by the server", "120", 5, 1000, 5, "Mbps",
            SettingTarget::MaxStreamingBitrate)
            .whenSetTo("playback/manualStreamingBitrate", "true"),
        toggleSpec("playback/unlimitedLocalBitrate", "Streaming", "No limit on the local network",
            "Applies when the server sees you as local", false, SettingTarget::UnlimitedLocalBitrate)
            .advanced(),
        toggleSpec("playback/preferRemux", "Streaming", "Prefer remuxing",
            "Repackages instead of re-encoding where it can", true, SettingTarget::PreferRemux)
            .advanced(),
        sliderSpec("playback/forwardCacheSizeMiB", "Streaming", "Read-ahead buffer",
            "Applies the next time something plays", "32", 16, 2048, 1, "MB", SettingTarget::ForwardCacheSize,
            SettingNormalizer::PowerOfTwoRange)
            .advanced(),

        selectSpec("subtitles/language", "Subtitles", "Preferred language", "Used when subtitles are picked for you",
            "", nullptr, 0, SettingTarget::SubtitleLanguage),
        selectSpec("subtitles/mode", "Subtitles", "When to show subtitles", "", "Default", kSubtitleModeChoices,
            SettingTarget::SubtitleMode),
        pageSpec("action/subtitleSettings", "Subtitles", "Subtitle appearance", "", SettingType::Action),

        // Shown by the subtitle appearance panel, which can sit over live video.
        selectSpec("subtitles/styling", "Subtitle Appearance", "Style",
            "Automatic styles plain text and leaves authored subtitles alone", "Auto", kSubtitleStylingChoices,
            SettingTarget::SubtitleStyling)
            .advanced(),
        sliderSpec("subtitles/scalePercent", "Subtitle Appearance", "Text Size", "Matches text and image subtitle size",
            "100", 50, 200, 5, "%", SettingTarget::SubtitleScale),
        sliderSpec("subtitles/verticalPositionPercent", "Subtitle Appearance", "Vertical position",
            "Higher values move subtitles up the screen", "95", 0, 100, 1, "%",
            SettingTarget::SubtitleVerticalPosition),
        toggleSpec("subtitles/alwaysOverridePositionAndSize", "Subtitle Appearance",
            "Always override size and position", "Also moves subtitles that place themselves", false,
            SettingTarget::SubtitlePositionAndSizeOverride),
        toggleSpec("subtitles/allowInBlackBars", "Subtitle Appearance", "Allow in black bars", "", true,
            SettingTarget::SubtitleAllowInBlackBars)
            .advanced(),
        toggleSpec("subtitles/overrideTextColor", "Subtitle Appearance", "Override text colour",
            "Use the selected colour instead of authored text colours", false,
            SettingTarget::SubtitleTextColorOverride),
        selectSpec("subtitles/textColor", "Subtitle Appearance", "Text colour", "", "#ffffff",
            kSubtitleTextColorChoices, SettingTarget::SubtitleTextColor, SettingNormalizer::SubtitleColor)
            .advanced(),
        selectSpec("subtitles/textWeight", "Subtitle Appearance", "Text weight", "Bold reads better on busy scenes",
            "normal", kSubtitleTextWeightChoices, SettingTarget::SubtitleTextWeight)
            .advanced(),
        selectSpec("subtitles/font", "Subtitle Appearance", "Font", "", "", kSubtitleFontChoices,
            SettingTarget::SubtitleFont, SettingNormalizer::SubtitleFont)
            .advanced(),
        selectSpec("subtitles/dropShadow", "Subtitle Appearance", "Outline", "", "", kSubtitleDropShadowChoices,
            SettingTarget::SubtitleDropShadow)
            .advanced(),
        selectSpec("subtitles/textBackground", "Subtitle Appearance", "Background", "", "transparent",
            kSubtitleBackgroundChoices, SettingTarget::SubtitleTextBackground)
            .advanced(),
        toggleSpec("subtitles/recolorImageSubtitles", "Subtitle Appearance", "Use text colour for images", "", false,
            SettingTarget::SubtitleRecolorImages)
            .advanced(),
        sliderSpec("subtitles/bitmapSharpnessPercent", "Subtitle Appearance", "Sharpness", "0% smoother · 100% sharper",
            "45", 0, 100, 5, "%", SettingTarget::SubtitleBitmapSharpness)
            .advanced(),
        toggleSpec("subtitles/bitmapShadowEnabled", "Subtitle Appearance", "Image shadow",
            "Add a two-layer contrast shadow to image subtitles", true, SettingTarget::SubtitleBitmapShadowEnabled)
            .advanced(),
        sliderSpec("subtitles/bitmapShadowCoreSize", "Subtitle Appearance", "Edge shadow softness",
            "Softness of the tight edge-protection layer", "1", 1, 4, 1, "px",
            SettingTarget::SubtitleBitmapShadowCoreSize)
            .advanced()
            .whenSetTo("subtitles/bitmapShadowEnabled", "true"),
        sliderSpec("subtitles/bitmapShadowCoreGrow", "Subtitle Appearance", "Edge shadow grow",
            "Expand the tight shadow beyond the subtitle edge", "1", 0, 4, 1, "px",
            SettingTarget::SubtitleBitmapShadowCoreGrow)
            .advanced()
            .whenSetTo("subtitles/bitmapShadowEnabled", "true"),
        sliderSpec("subtitles/bitmapShadowCoreOpacityPercent", "Subtitle Appearance", "Edge shadow strength", "", "70",
            0, 100, 5, "%", SettingTarget::SubtitleBitmapShadowCoreOpacity)
            .advanced()
            .whenSetTo("subtitles/bitmapShadowEnabled", "true"),
        toggleSpec("subtitles/bitmapShadowSpreadEnabled", "Subtitle Appearance", "Wide shadow",
            "Add a soft offset layer behind the edge shadow", true, SettingTarget::SubtitleBitmapShadowSpreadEnabled)
            .advanced()
            .whenSetTo("subtitles/bitmapShadowEnabled", "true"),
        sliderSpec("subtitles/bitmapShadowSpreadSize", "Subtitle Appearance", "Wide shadow softness", "", "6", 1, 16, 1,
            "px", SettingTarget::SubtitleBitmapShadowSpreadSize)
            .advanced()
            .whenSetTo("subtitles/bitmapShadowEnabled", "true"),
        sliderSpec("subtitles/bitmapShadowSpreadGrow", "Subtitle Appearance", "Wide shadow grow", "", "0", 0, 8, 1,
            "px", SettingTarget::SubtitleBitmapShadowSpreadGrow)
            .advanced()
            .whenSetTo("subtitles/bitmapShadowEnabled", "true"),
        sliderSpec("subtitles/bitmapShadowSpreadX", "Subtitle Appearance", "Wide shadow horizontal offset", "", "2",
            -16, 16, 1, "px", SettingTarget::SubtitleBitmapShadowSpreadX)
            .advanced()
            .whenSetTo("subtitles/bitmapShadowEnabled", "true"),
        sliderSpec("subtitles/bitmapShadowSpreadY", "Subtitle Appearance", "Wide shadow vertical offset", "", "3", -16,
            16, 1, "px", SettingTarget::SubtitleBitmapShadowSpreadY)
            .advanced()
            .whenSetTo("subtitles/bitmapShadowEnabled", "true"),
        sliderSpec("subtitles/bitmapShadowSpreadOpacityPercent", "Subtitle Appearance", "Wide shadow strength", "",
            "30", 0, 100, 5, "%", SettingTarget::SubtitleBitmapShadowSpreadOpacity)
            .advanced()
            .whenSetTo("subtitles/bitmapShadowEnabled", "true"),
        toggleSpec("subtitles/bitmapShadowDither", "Subtitle Appearance", "Shadow dithering",
            "Reduce banding in wide shadow gradients", true, SettingTarget::SubtitleBitmapShadowDither)
            .advanced()
            .whenSetTo("subtitles/bitmapShadowEnabled", "true"),
        sliderSpec("subtitles/hdrBrightnessPercent", "Subtitle Appearance", "HDR Subtitle Brightness",
            "100% keeps the original brightness", "50", 5, 100, 5, "%", SettingTarget::SubtitleHdrBrightness)
            .advanced()
            .duringHdrPlayback(),
        pageSpec("action/resetSubtitleAppearance", "Subtitle Appearance", "Reset appearance", "", SettingType::Action)
            .advanced(),

        selectSpec(
            "input/redButton", "Remote buttons", "Red", "", "none", kButtonActionChoices, SettingTarget::RedButton)
            .expert()
            .onWebOS(),
        selectSpec("input/greenButton", "Remote buttons", "Green", "", "skipBackAndEnableSubs", kButtonActionChoices,
            SettingTarget::GreenButton)
            .expert()
            .onWebOS(),
        selectSpec("input/yellowButton", "Remote buttons", "Yellow", "", "none", kButtonActionChoices,
            SettingTarget::YellowButton)
            .expert()
            .onWebOS(),
        selectSpec(
            "input/blueButton", "Remote buttons", "Blue", "", "none", kButtonActionChoices, SettingTarget::BlueButton)
            .expert()
            .onWebOS(),

        // Only platforms with an in-app installer expose this preference.
        toggleSpec("updates/automatic", "Updates", "Automatic updates", "", true, SettingTarget::AutomaticUpdates)
#if defined(SPOOL_WEBOS)
            .onWebOS(),
#else
            .onAndroid(),
#endif

        pageSpec("action/accounts", "Accounts", "Accounts", "", SettingType::Action),
        pageSpec("action/providers", "Accounts", "Providers", "", SettingType::Action),
        selectSpec("providers/updates", "Accounts", "Provider updates", "", "ask", kProviderUpdateChoices,
            SettingTarget::External),
        pageSpec("action/manageCertificates", "Accounts", "Remembered certificates", "", SettingType::Action),

        pageSpec("action/exportDiagnostics", "Diagnostics", "Export diagnostics", "", SettingType::Action),
        pageSpec("action/clearLogs", "Diagnostics", "Clear logs", "", SettingType::Action),
        pageSpec("shell/diagnostics", "Diagnostics", "Diagnostics overlay", "", SettingType::Toggle).expert(),
        toggleSpec("settings/toneMappingVisualization", "Diagnostics", "Tone mapping overlay",
            "False-colour view of how HDR is mapped", false, SettingTarget::ToneMappingVisualization)
            .expert()
            .onDesktop(),
        pageSpec("shell/latencyGuard", "Diagnostics", "Record input latency", "", SettingType::Toggle).expert(),
        pageSpec("shell/latencyOverlay", "Diagnostics", "Warn about slow input", "", SettingType::Toggle).expert(),
        pageSpec("action/clearLatencyStatistics", "Diagnostics", "Clear latency samples", "", SettingType::Action)
            .expert(),

        pageSpec("about/version", "About", "Spool", "", SettingType::ReadOnly),
        pageSpec("about/locale", "About", "Active language", "", SettingType::ReadOnly),
        pageSpec("action/openSourceNotices", "About", "Open-source notices", "", SettingType::Action),
    };
    return specs;
}

const SettingSpec *findSettingSpec(const QString& key)
{
    const auto& specs = settingSpecs();
    const auto it = std::find_if(
        specs.cbegin(), specs.cend(), [&key](const SettingSpec& spec) { return key == QLatin1String(spec.key); });
    return it == specs.cend() ? nullptr : &(*it);
}

QVariant settingDefaultValue(const SettingSpec& spec)
{
    if (spec.target == SettingTarget::UiScale)
        return platformDefaultUiScalePercent();

    switch (spec.type) {
    case SettingType::Toggle:
        return boolValue(QLatin1String(spec.defaultValue));
    case SettingType::Slider:
        return std::clamp(QString::fromLatin1(spec.defaultValue).toInt(), spec.minimum, spec.maximum);
    case SettingType::Action:
    case SettingType::ReadOnly:
    case SettingType::Text:
    case SettingType::Select:
        break;
    }
    return QString::fromLatin1(spec.defaultValue);
}

QVariant normalizedSettingValue(const SettingSpec& spec, const QVariant& value)
{
    switch (spec.normalizer) {
    case SettingNormalizer::Bool:
        return boolValue(value);
    case SettingNormalizer::IntRange:
        return std::clamp(value.toString().toInt(), spec.minimum, spec.maximum);
    case SettingNormalizer::PowerOfTwoRange: {
        const int clamped = std::clamp(value.toString().toInt(), spec.minimum, spec.maximum);
        return 1 << std::lround(std::log2(clamped));
    }
    case SettingNormalizer::AudioOutput:
        return normalizedAudioOutputMode(value.toString());
    case SettingNormalizer::Choice:
        return choiceOrDefault(spec, value.toString());

    case SettingNormalizer::SubtitleFont: {
        // Desktop offers installed families on top of the bundled ones, so a
        // "system:" family is valid even though it is not a declared choice.
        const QString font = value.toString();
        return font.startsWith(QStringLiteral("system:")) && font.size() > 7 ? font : choiceOrDefault(spec, font);
    }
    case SettingNormalizer::SubtitleColor:
        return normalizedSubtitleColor(value.toString());
    case SettingNormalizer::String:
        break;
    }
    return value.toString();
}

QString serializedSettingValue(const SettingSpec& spec, const QVariant& value)
{
    const QVariant normalized = normalizedSettingValue(spec, value);
    if (spec.type == SettingType::Toggle)
        return normalized.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (spec.type == SettingType::Slider)
        return QString::number(normalized.toInt());
    return normalized.toString();
}

QVariantList settingSchemaModel()
{
    QVariantList model;
    model.reserve(settingSpecs().size());
    for (const SettingSpec& spec : settingSpecs()) {
        QVariantMap row { { QStringLiteral("key"), QLatin1String(spec.key) },
            { QStringLiteral("source"), spec.persisted ? QStringLiteral("settings") : QStringLiteral("page") },
            { QStringLiteral("group"), QLatin1String(spec.group) },
            { QStringLiteral("title"), QLatin1String(spec.title) },
            { QStringLiteral("description"), QLatin1String(spec.description) },
            { QStringLiteral("type"), typeName(spec.type) },
            { QStringLiteral("defaultValue"), settingDefaultValue(spec) },
            { QStringLiteral("level"), static_cast<int>(spec.level) },
            { QStringLiteral("platform"), platformName(spec.platform) },
            { QStringLiteral("dependsOnKey"), QLatin1String(spec.dependsOnKey) },
            { QStringLiteral("dependsOnValue"), QLatin1String(spec.dependsOnValue) },
            { QStringLiteral("requiresHdrPlayback"), spec.requiresHdrPlayback },
            { QStringLiteral("from"), spec.minimum }, { QStringLiteral("to"), spec.maximum },
            { QStringLiteral("step"), spec.step }, { QStringLiteral("unitText"), QLatin1String(spec.unit) } };
        QVariantList values;
        QVariantList labels;
        values.reserve(spec.choiceCount);
        labels.reserve(spec.choiceCount);
        for (qsizetype i = 0; i < spec.choiceCount; ++i) {
            values.push_back(QLatin1String(spec.choices[i].value));
            labels.push_back(QLatin1String(spec.choices[i].label));
        }
        row.insert(QStringLiteral("choiceValues"), values);
        row.insert(QStringLiteral("choiceLabels"), labels);
        model.push_back(row);
    }
    return model;
}

} // namespace Spool
