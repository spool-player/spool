#include "platform/PlatformPlaybackSurface.h"

#include "player/MpvVideoItem.h"

#include <QDebug>
#include <QMetaObject>
#include <QObject>
#include <QtGlobal>

namespace Spool {
namespace {
    QMetaObject::Connection g_renderErrorConnection;
}

bool platformIdleMpvPreparationEnabled()
{
    return false;
}
void runAfterPlatformMpvLoaded(std::function<void()> callback)
{
    callback();
}
MpvOptionProfile::Platform platformMpvOptionProfile()
{
#if defined(Q_OS_ANDROID)
    return MpvOptionProfile::Platform::Android;
#else
    return MpvOptionProfile::Platform::Desktop;
#endif
}
bool platformUsesEmbeddedVideo(const PlaybackSession&, bool)
{
    return true;
}
QString platformPlaybackBackendName(bool)
{
    return QStringLiteral("desktop");
}

bool configurePlatformMpvSurface(mpv_handle *, NativeAppWindow&, bool, bool, QString&)
{
    return true;
}

bool attachPlatformMpvSurface(mpv_handle *handle, bool needsVideoSurface, bool, QObject& context,
    std::function<void(const QString&)> errorHandler, QString& errorMessage)
{
    if (!needsVideoSurface)
        return true;
    auto *videoItem = MpvVideoItem::instance();
    if (!videoItem) {
        qCritical() << "playback surface: MpvVideoItem instance is missing";
        errorMessage = QStringLiteral("The video surface is unavailable. Return to the library and try again.");
        return false;
    }
    QObject::disconnect(g_renderErrorConnection);
    g_renderErrorConnection = QObject::connect(videoItem, &MpvVideoItem::renderError, &context,
        [errorHandler = std::move(errorHandler)](const QString& message) { errorHandler(message); });
    videoItem->setMpvHandle(handle);
    return true;
}

bool waitForPlatformMpvSurfaceReady(bool needsVideoSurface, bool, QString& errorMessage)
{
    if (!needsVideoSurface)
        return true;
    auto *videoItem = MpvVideoItem::instance();
    if (videoItem && videoItem->waitForRenderContext())
        return true;
    errorMessage = QStringLiteral("The video renderer did not become ready. Return to the library and try again.");
    return false;
}

bool releasePlatformMpvSurface(bool)
{
    auto *videoItem = MpvVideoItem::instance();
    if (!videoItem)
        return true;
    return videoItem->releaseMpvHandle();
}

QString platformPreparingStatus(bool needsVideoSurface, bool)
{
    return needsVideoSurface ? QStringLiteral("Preparing libmpv...") : QStringLiteral("Preparing audio...");
}

bool applyPlatformSubtitlePreload(mpv_handle *, const PlaybackSession&, const QString&, QString&)
{
    return true;
}
bool platformUsesBackgroundPlaybackPolicy()
{
    return false;
}
void platformAudioTrackChanged(int) { }
void platformVideoSizeChanged(int, int) { }

} // namespace Spool
