#include "platform/PlatformPlaybackSurface.h"

#include "platform/NativeAppWindow.h"
#include "platform/webos/WebOSMpvRuntime.h"
#include "player/MpvVideoItem.h"

#include <QDebug>
#include <QMetaObject>
#include <QObject>

#include <mpv/client.h>

namespace Spool {
namespace {
    QMetaObject::Connection g_renderErrorConnection;

    bool setRequiredProperty(mpv_handle *handle, const char *name, const QByteArray& value)
    {
        const int result = mpv_set_property_string(handle, name, value.constData());
        if (result >= 0)
            return true;
        qCritical() << "playback surface: failed to set required property" << name << mpv_error_string(result);
        return false;
    }
}

bool platformIdleMpvPreparationEnabled()
{
    const bool enabled = qEnvironmentVariable("SPOOL_DISABLE_IDLE_MPV") != QLatin1String("1");
    qInfo() << "player: idle mpv preparation" << (enabled ? "enabled" : "disabled");
    return enabled;
}

void runAfterPlatformMpvLoaded(std::function<void()> callback)
{
    WebOSMpvRuntime::runAfterLoaded(std::move(callback));
}

MpvOptionProfile::Platform platformMpvOptionProfile()
{
    return MpvOptionProfile::Platform::WebOS;
}
bool platformUsesEmbeddedVideo(const PlaybackSession& session, bool)
{
    return MpvOptionProfile::useWebOSSoftwareVideo(session);
}

QString platformPlaybackBackendName(bool embeddedVideo)
{
    return embeddedVideo ? QStringLiteral("webOS OpenGL software") : QStringLiteral("webOS Starfish");
}

bool configurePlatformMpvSurface(
    mpv_handle *handle, NativeAppWindow& window, bool needsVideoSurface, bool embeddedVideo, QString& errorMessage)
{
    if (!needsVideoSurface || embeddedVideo)
        return true;
    if (setRequiredProperty(handle, "vo-starfish-window-id", window.windowId().toUtf8())
        && setRequiredProperty(handle, "vo-starfish-window-width", QByteArray::number(window.width()))
        && setRequiredProperty(handle, "vo-starfish-window-height", QByteArray::number(window.height()))) {
        return true;
    }
    errorMessage = QStringLiteral("Failed to configure the native video surface.");
    return false;
}

bool attachPlatformMpvSurface(mpv_handle *handle, bool needsVideoSurface, bool embeddedVideo, QObject& context,
    std::function<void(const QString&)> errorHandler, QString& errorMessage)
{
    if (!needsVideoSurface || !embeddedVideo)
        return true;
    auto *videoItem = MpvVideoItem::instance();
    if (!videoItem) {
        qCritical() << "playback surface: MpvVideoItem instance is missing";
        errorMessage
            = QStringLiteral("The software video surface is unavailable. Return to the library and try again.");
        return false;
    }
    QObject::disconnect(g_renderErrorConnection);
    g_renderErrorConnection = QObject::connect(videoItem, &MpvVideoItem::renderError, &context,
        [errorHandler = std::move(errorHandler)](const QString& message) { errorHandler(message); });
    videoItem->setMpvHandle(handle);
    return true;
}

bool waitForPlatformMpvSurfaceReady(bool needsVideoSurface, bool embeddedVideo, QString& errorMessage)
{
    if (!needsVideoSurface || !embeddedVideo)
        return true;
    auto *videoItem = MpvVideoItem::instance();
    if (videoItem && videoItem->waitForRenderContext())
        return true;
    errorMessage
        = QStringLiteral("The software video renderer did not become ready. Return to the library and try again.");
    return false;
}

bool releasePlatformMpvSurface(bool embeddedVideo)
{
    if (!embeddedVideo)
        return true;
    auto *videoItem = MpvVideoItem::instance();
    return !videoItem || videoItem->releaseMpvHandle();
}

QString platformPreparingStatus(bool needsVideoSurface, bool embeddedVideo)
{
    if (!needsVideoSurface)
        return QStringLiteral("Preparing audio...");
    return embeddedVideo ? QStringLiteral("Preparing software video...")
                         : QStringLiteral("Preparing libmpv + Starfish...");
}

bool applyPlatformSubtitlePreload(
    mpv_handle *handle, const PlaybackSession& session, const QString& preferredLanguage, QString& errorMessage)
{
    const QByteArray streams = MpvOptionProfile::preloadedSubtitleStreams(session, preferredLanguage);
    if (mpv_set_property_string(handle, "demuxer-preload-subtitle-streams", streams.constData()) < 0) {
        errorMessage = QStringLiteral("libmpv rejected the subtitle preload request.");
        return false;
    }
    qInfo() << "player: requested subtitle packet preload streams="
            << (streams.isEmpty() ? QByteArrayLiteral("none") : streams) << "language=" << preferredLanguage;
    return true;
}

bool platformUsesBackgroundPlaybackPolicy()
{
    return true;
}
void platformAudioTrackChanged(int index)
{
    qInfo() << "player: webOS audio track changed" << index;
}
// Starfish is told the video's shape through its own media pipeline, so
// there is no surface here for this to reshape.
void platformVideoSizeChanged(int, int) { }

} // namespace Spool
