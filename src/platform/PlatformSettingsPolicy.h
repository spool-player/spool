#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace Spool {

struct SettingChoice;

struct PlatformAudioOutputPolicy {
    const SettingChoice *choices = nullptr;
    qsizetype choiceCount = 0;
    const char *defaultValue = "auto";
};

const PlatformAudioOutputPolicy& platformAudioOutputPolicy();
QString normalizedPlatformAudioOutputMode(const QString& mode);
QStringList platformSystemSubtitleFonts();

int platformDefaultUiScalePercent();
// Which image codec this device should ask the server for. Decoding WebP costs
// roughly three times what an equivalent JPEG costs, measured on a 2018 LG TV,
// and a TV has neither the cores to hide that nor a hardware image decoder to
// take it off the CPU. Everything with a desktop-class CPU spends the cycles
// and takes the smaller download instead.
const char *platformDefaultArtworkFormat();
// Which picture-quality rung a device starts on before it has shown what it
// can actually sustain. Coarse on purpose: the frame-drop measurement over the
// first seconds of playback is what really decides, so this only has to be
// close enough that most devices are never corrected.
const char *platformDefaultRenderQuality();
// Whether this platform can hand the decoder a surface of its own and leave
// the display pipeline to scale and present it. Where it can, that is the rung
// below the cheapest GPU profile rather than a dead end.
bool platformSupportsDirectVideoOutput();
// Which of those two a device starts on. A part that cannot shade a 4K frame
// sixty times a second should never have been asked to in the first place, so
// it is handed the display pipeline from the outset rather than being demoted
// to it after a viewer has watched the first few seconds stutter.
const char *platformDefaultVideoOutput();
bool platformUsesPerOutputAudioDelay();
bool platformDefaultRemoteControlTargetEnabled();
QString normalizedPlatformAudioRoute(const QString& output);
QString platformAudioRouteDisplayName(const QString& output);
QString platformAudioDelayStorageKey(const QString& output);
int platformAutomaticAudioDelayMs(const QString& output, int displayLatencyMs, int outputLatencyMs);

} // namespace Spool
