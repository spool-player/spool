#include "platform/PlatformSettingsPolicy.h"

#include "app/SettingsSchema.h"
#include "platform/webos/WebOSAudioSyncPolicy.h"

namespace Spool {
namespace {
    constexpr SettingChoice kChoices[]
        = { { "pulse", "PulseAudio" }, { "alsa", "ALSA" }, { "starfish-pcm", "Starfish" } };
}

const PlatformAudioOutputPolicy& platformAudioOutputPolicy()
{
    static const PlatformAudioOutputPolicy policy { kChoices, 3, "pulse" };
    return policy;
}

QString normalizedPlatformAudioOutputMode(const QString& mode)
{
    if (mode == QStringLiteral("starfish") || mode == QStringLiteral("starfish-pcm"))
        return QStringLiteral("starfish-pcm");
    return mode == QStringLiteral("alsa") ? mode : QStringLiteral("pulse");
}

QStringList platformSystemSubtitleFonts()
{
    return {};
}

int platformDefaultUiScalePercent()
{
    return 100;
}

const char *platformDefaultArtworkFormat()
{
    return "jpeg";
}

const char *platformDefaultRenderQuality()
{
    // Starfish decodes and presents; libplacebo is only reached for legacy
    // codecs, and that path already carries a profile of its own.
    return "fast";
}

bool platformSupportsDirectVideoOutput()
{
    return false;
}

const char *platformDefaultVideoOutput()
{
    return "enhanced";
}
bool platformUsesPerOutputAudioDelay()
{
    return true;
}
bool platformDefaultRemoteControlTargetEnabled()
{
    return true;
}
QString normalizedPlatformAudioRoute(const QString& output)
{
    return AudioSyncPolicy::normalizedOutputKey(output);
}
QString platformAudioRouteDisplayName(const QString& output)
{
    return AudioSyncPolicy::outputDisplayName(output);
}
QString platformAudioDelayStorageKey(const QString& output)
{
    return AudioSyncPolicy::delayStorageKey(output);
}
int platformAutomaticAudioDelayMs(const QString& output, int displayLatencyMs, int outputLatencyMs)
{
    return AudioSyncPolicy::automaticBaseDelayMs(output, displayLatencyMs, outputLatencyMs);
}

} // namespace Spool
