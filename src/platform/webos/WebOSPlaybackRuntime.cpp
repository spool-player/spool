#include "platform/PlatformPlaybackRuntime.h"

#include "platform/webos/WebOSMpvRuntime.h"

#include <QDebug>
#include <QMetaObject>
#include <QPointer>

namespace JellyfinNative {
namespace {

    const QStringList kSoftwareVideoCodecs { QStringLiteral("mpeg1video"), QStringLiteral("mpeg2video"),
        QStringLiteral("mpeg4"), QStringLiteral("h263"), QStringLiteral("vc1") };

    QStringList withSoftwareVideoCodecs(QStringList codecs)
    {
        for (const QString& codec : kSoftwareVideoCodecs) {
            if (!codecs.contains(codec, Qt::CaseInsensitive))
                codecs.push_back(codec);
        }
        return codecs;
    }

} // namespace

qint64 platformAudioDecodeCpuTimeNs()
{
    return WebOSMpvRuntime::audioDecodeCpuTimeNs();
}

void configurePlatformPlaybackCapabilities(VideoCodecCapabilityApplier apply, QObject& callbackContext)
{
    apply(withSoftwareVideoCodecs({ QStringLiteral("h264") }), true);
    QPointer<QObject> guardedContext(&callbackContext);
    WebOSMpvRuntime::probeStarfishVideoCodecsAsync([apply, guardedContext](const QStringList& codecs) {
        if (!guardedContext)
            return;
        QMetaObject::invokeMethod(
            guardedContext,
            [apply, codecs] {
                if (codecs.isEmpty()) {
                    qWarning() << "playback capabilities: Starfish probe returned no codecs; retaining software set";
                    return;
                }
                apply(withSoftwareVideoCodecs(codecs), true);
            },
            Qt::QueuedConnection);
    });
}

} // namespace JellyfinNative
