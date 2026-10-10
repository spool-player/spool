#include "platform/PlatformPlaybackRuntime.h"

#include <mpv/client.h>

namespace Spool {

qint64 platformAudioDecodeCpuTimeNs()
{
    return static_cast<qint64>(mpv_get_audio_decode_cpu_time_ns());
}

void configurePlatformPlaybackCapabilities(VideoCodecCapabilityApplier, QObject&) { }

} // namespace Spool
