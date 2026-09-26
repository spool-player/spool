#include "platform/PlatformPlaybackSurface.h"

#include "platform/NativeAppWindow.h"
#include "player/MpvVideoItem.h"

#include <QCoreApplication>
#include <QDebug>
#include <QJniObject>
#include <QMetaObject>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QWaitCondition>

#include <mpv/client.h>

namespace Spool {
namespace {
    constexpr const char *kBridge = "com/sachk/spool/VideoSurfaceBridge";
    // The Android UI thread regularly blocks on the Qt thread, so waiting on
    // it is a deadlock waiting to happen. Bounded, so a surface that never
    // arrives fails the playback rather than hanging the app.
    constexpr int kSurfaceWaitMs = 3000;

    QMetaObject::Connection g_renderErrorConnection;
    // Held so the window's opaque background can be put back when direct
    // playback ends. Nothing else has both the window and the moment.
    QPointer<NativeAppWindow> g_underlaidWindow;

    struct DirectSurface {
        QMutex mutex;
        QWaitCondition arrived;
        // A global reference: the local one the callback is handed dies with
        // the JNI frame, and mpv keeps the Surface for the life of the VO.
        QJniObject surface;
        bool present = false;
    };

    DirectSurface& directSurface()
    {
        static DirectSurface surface;
        return surface;
    }

    QJniObject activity()
    {
        return QNativeInterface::QAndroidApplication::context();
    }
}

extern "C" JNIEXPORT void JNICALL Java_com_sachk_spool_VideoSurfaceBridge_nativeSurfaceReady(
    JNIEnv *, jclass, jobject surface)
{
    DirectSurface& state = directSurface();
    const QMutexLocker locker(&state.mutex);
    state.surface = QJniObject(surface);
    state.present = state.surface.isValid();
    qInfo() << "playback surface: android video surface ready" << state.present;
    state.arrived.wakeAll();
}

extern "C" JNIEXPORT void JNICALL Java_com_sachk_spool_VideoSurfaceBridge_nativeSurfaceResized(
    JNIEnv *, jclass, jint width, jint height)
{
    qInfo() << "playback surface: android video surface" << width << "x" << height;
}

extern "C" JNIEXPORT void JNICALL Java_com_sachk_spool_VideoSurfaceBridge_nativeSurfaceLost(JNIEnv *, jclass)
{
    DirectSurface& state = directSurface();
    const QMutexLocker locker(&state.mutex);
    state.present = false;
    state.surface = QJniObject();
    qInfo() << "playback surface: android video surface lost";
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
    return MpvOptionProfile::Platform::Android;
}

bool platformUsesEmbeddedVideo(const PlaybackSession&, bool directRequested)
{
    // Embedded means "through the Qt scene graph". Direct is the opposite of
    // it, and the only thing that decides is what the viewer asked for.
    return !directRequested;
}

QString platformPlaybackBackendName(bool embeddedVideo)
{
    return embeddedVideo ? QStringLiteral("libmpv OpenGL ES") : QStringLiteral("MediaCodec surface");
}

bool configurePlatformMpvSurface(
    mpv_handle *handle, NativeAppWindow& window, bool needsVideoSurface, bool embeddedVideo, QString& errorMessage)
{
    if (!needsVideoSurface || embeddedVideo)
        return true;

    DirectSurface& state = directSurface();
    {
        const QMutexLocker locker(&state.mutex);
        state.present = false;
        state.surface = QJniObject();
    }
    const QJniObject context = activity();
    if (!context.isValid()) {
        errorMessage = QStringLiteral("The video surface is unavailable. Return to the library and try again.");
        return false;
    }
    QJniObject::callStaticMethod<void>(kBridge, "show", "(Landroid/app/Activity;)V", context.object<jobject>());

    QJniObject surface;
    {
        QMutexLocker locker(&state.mutex);
        if (!state.present)
            state.arrived.wait(&state.mutex, kSurfaceWaitMs);
        surface = state.surface;
        if (!state.present || !surface.isValid()) {
            locker.unlock();
            qCritical() << "playback surface: android video surface did not arrive";
            errorMessage
                = QStringLiteral("The video surface did not become ready. Return to the library and try again.");
            return false;
        }
    }

    // wid is UPDATE_VO rather than fixed, so setting it here -- after
    // mpv_initialize, like webOS sets its Starfish window -- still reaches the
    // VO when it is created.
    auto wid = static_cast<int64_t>(reinterpret_cast<intptr_t>(surface.object<jobject>()));
    if (mpv_set_property(handle, "wid", MPV_FORMAT_INT64, &wid) < 0) {
        errorMessage = QStringLiteral("Failed to configure the native video surface.");
        return false;
    }
    // The video output places nothing itself -- the Java view is sized to the
    // frame and centred -- but it still has to know the window to work out
    // where subtitles belong, black bars included. In panel pixels, which is
    // what it rasterises the OSD in.
    const qreal ratio = window.devicePixelRatio() > 0 ? window.devicePixelRatio() : 1.0;
    const auto windowWidth = QByteArray::number(qRound(window.width() * ratio));
    const auto windowHeight = QByteArray::number(qRound(window.height() * ratio));
    mpv_set_property_string(handle, "vo-mediacodec-embed-window-width", windowWidth.constData());
    mpv_set_property_string(handle, "vo-mediacodec-embed-window-height", windowHeight.constData());

    // Nothing Qt paints may hide the video plane underneath it.
    window.setVideoUnderlayActive(true);
    g_underlaidWindow = &window;
    return true;
}

bool attachPlatformMpvSurface(mpv_handle *handle, bool needsVideoSurface, bool embeddedVideo, QObject& context,
    std::function<void(const QString&)> errorHandler, QString& errorMessage)
{
    if (!needsVideoSurface || !embeddedVideo)
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

bool waitForPlatformMpvSurfaceReady(bool needsVideoSurface, bool embeddedVideo, QString& errorMessage)
{
    if (!needsVideoSurface || !embeddedVideo)
        return true;
    auto *videoItem = MpvVideoItem::instance();
    if (videoItem && videoItem->waitForRenderContext())
        return true;
    errorMessage = QStringLiteral("The video renderer did not become ready. Return to the library and try again.");
    return false;
}

bool releasePlatformMpvSurface(bool embeddedVideo)
{
    if (!embeddedVideo) {
        // Reverse of the order it was brought up in: mpv has already been torn
        // down by the time this runs, so the Surface has no reader left before
        // the view goes away.
        DirectSurface& state = directSurface();
        {
            const QMutexLocker locker(&state.mutex);
            state.present = false;
            state.surface = QJniObject();
        }
        if (g_underlaidWindow) {
            g_underlaidWindow->setVideoUnderlayActive(false);
            g_underlaidWindow = nullptr;
        }
        const QJniObject context = activity();
        if (context.isValid()) {
            QJniObject::callStaticMethod<void>(kBridge, "hide", "(Landroid/app/Activity;)V", context.object<jobject>());
        }
        return true;
    }
    auto *videoItem = MpvVideoItem::instance();
    return !videoItem || videoItem->releaseMpvHandle();
}

QString platformPreparingStatus(bool needsVideoSurface, bool embeddedVideo)
{
    if (!needsVideoSurface)
        return QStringLiteral("Preparing audio...");
    return embeddedVideo ? QStringLiteral("Preparing libmpv...") : QStringLiteral("Preparing direct playback...");
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

void platformVideoSizeChanged(int width, int height)
{
    if (!g_underlaidWindow || width <= 0 || height <= 0)
        return;
    const QJniObject context = activity();
    if (!context.isValid())
        return;
    QJniObject::callStaticMethod<void>(kBridge, "setVideoSize", "(Landroid/app/Activity;II)V",
        context.object<jobject>(), static_cast<jint>(width), static_cast<jint>(height));
}

} // namespace Spool
