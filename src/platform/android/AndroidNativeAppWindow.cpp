#include "platform/NativeAppWindow.h"

#include <QCoreApplication>
#include <QExposeEvent>
#include <QJniObject>
#include <QResizeEvent>

#include <video/out/android_overlay.h>

namespace Spool {

struct NativeAppWindow::PlatformData {
    // Where mpv draws subtitles when the video plane is not ours to draw on.
    // MediaCodec owns that surface; this is the layer Qt already keeps above
    // it, reached by exactly the two calls the Starfish output uses on webOS.
    struct OverlayImageBuffer {
        QImage image;
        int x = 0;
        int y = 0;
    };

    explicit PlatformData(NativeAppWindow *window)
        : owner(window)
    {
    }

    NativeAppWindow *owner = nullptr;

    static uint8_t *overlayAcquire(void *data, int x, int y, int width, int height, int *stride, void **buffer)
    {
        auto *platform = static_cast<PlatformData *>(data);
        if (!platform || !platform->owner || !stride || !buffer || width <= 0 || height <= 0)
            return nullptr;
        static_assert(Q_BYTE_ORDER == Q_LITTLE_ENDIAN, "OSD direct path assumes little-endian QImage layout");
        auto *frame = new OverlayImageBuffer { QImage(width, height, QImage::Format_ARGB32_Premultiplied), x, y };
        if (frame->image.isNull()) {
            delete frame;
            return nullptr;
        }
        *stride = frame->image.bytesPerLine();
        *buffer = frame;
        return frame->image.bits();
    }

    static void overlayPresent(void *data, void *buffer, bool visible)
    {
        auto *platform = static_cast<PlatformData *>(data);
        auto *frame = static_cast<OverlayImageBuffer *>(buffer);
        if (!platform || !platform->owner) {
            delete frame;
            return;
        }
        if (!visible || !frame || frame->image.isNull()) {
            delete frame;
            platform->owner->scheduleOverlayImage({});
            return;
        }
        platform->owner->scheduleOverlayImage(std::move(frame->image), frame->x, frame->y);
        delete frame;
    }
};

NativeAppWindow::NativeAppWindow(const QString& appId, QWindow *parent)
    : QQuickView(parent)
    , m_appId(appId)
    , m_platform(std::make_unique<PlatformData>(this))
{
    setColor(Qt::black);
    setResizeMode(QQuickView::SizeRootObjectToView);
    setTitle(QStringLiteral("Spool"));
    android_overlay_set_callbacks(&PlatformData::overlayAcquire, &PlatformData::overlayPresent, m_platform.get());
}

NativeAppWindow::~NativeAppWindow()
{
    android_overlay_set_callbacks(nullptr, nullptr, nullptr);
}

void NativeAppWindow::setVideoUnderlayActive(bool active)
{
    // Black is right for everything else: it is what the launch screen hands
    // over to and what letterboxing should be. During direct playback it is
    // the one thing that would hide the video plane underneath.
    setColor(active ? QColor(0, 0, 0, 0) : Qt::black);
}

bool NativeAppWindow::prepareForUiSurface()
{
    if (!isVisible())
        setImmersive(false);
    requestActivate();
    return true;
}

bool NativeAppWindow::prepareForPlaybackSurface()
{
    return prepareForUiSurface();
}

void NativeAppWindow::bringToFront()
{
    if (!isVisible())
        setImmersive(m_immersive);
    requestActivate();
}

void NativeAppWindow::toggleFullScreen()
{
    setImmersive(!m_immersive);
}

// Browsing shares the screen with the system: the clock, the notifications
// and the gesture bar all stay where the user expects them, and the interface
// is inset to clear them. Only playback takes the whole panel.
void NativeAppWindow::setImmersive(bool immersive)
{
    m_immersive = immersive;
    if (immersive)
        showFullScreen();
    else
        showMaximized();
}

// Back at the top of the stack is how an Android app is left, and until now
// nothing here had anywhere to send it: the press did nothing and the only way
// out was the launcher itself. Finishing the task rather than quitting the
// process is what the system expects -- the app leaves the screen at once, and
// Android decides when to reclaim it.
void NativeAppWindow::exitToLauncher()
{
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread([]() {
        QJniObject activity = QNativeInterface::QAndroidApplication::context();
        if (activity.isValid())
            activity.callMethod<void>("finishAndRemoveTask");
    });
}

QString NativeAppWindow::windowId() const
{
    return {};
}

void NativeAppWindow::exposeEvent(QExposeEvent *event)
{
    QQuickView::exposeEvent(event);
}

void NativeAppWindow::resizeEvent(QResizeEvent *event)
{
    QQuickView::resizeEvent(event);
}

void NativeAppWindow::handlePlatformSurfaceCreated() { }
void NativeAppWindow::handlePlatformSurfaceAboutToBeDestroyed() { }

} // namespace Spool
