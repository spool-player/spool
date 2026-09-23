#pragma once

#include <QStringList>
#include <QtGlobal>

#include <functional>

class QObject;

namespace Spool {

qint64 platformAudioDecodeCpuTimeNs();

// Hands the platform's decodable video codecs to whoever negotiates
// playback. The platform may call the applier more than once: webOS reports
// a software set immediately and the hardware set once its probe answers,
// marshalled onto callbackContext's thread.
using VideoCodecCapabilityApplier = std::function<void(const QStringList& videoCodecs, bool restrictVideoCodecs)>;
void configurePlatformPlaybackCapabilities(VideoCodecCapabilityApplier apply, QObject& callbackContext);

} // namespace Spool
