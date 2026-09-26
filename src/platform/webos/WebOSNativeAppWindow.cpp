#include "platform/NativeAppWindow.h"

#include <QColor>
#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QExposeEvent>
#include <QGuiApplication>
#include <QMetaObject>
#include <QMutexLocker>
#include <QResizeEvent>
#include <QThread>
#include <QTimer>

#include <qpa/qplatformnativeinterface.h>

#include <algorithm>
#include <cstring>

#include <cstdlib>
#include <string>

extern "C" {
#include "video/out/starfish/starfish_ctx.h"
}
#include <wayland-client.h>
#include <wayland-webos-foreign-client-protocol.h>
#include <wayland-webos-shell-client-protocol.h>

namespace Spool {

namespace {

    struct OverlayImageBuffer {
        QImage image;
        int x = 0;
        int y = 0;
    };

} // namespace

struct NativeAppWindow::PlatformData {
    struct CropRegion {
        int origW = 0;
        int origH = 0;
        int srcX = 0;
        int srcY = 0;
        int srcW = 0;
        int srcH = 0;
        int dstX = 0;
        int dstY = 0;
        int dstW = 0;
        int dstH = 0;
        bool valid = false;
    };

    explicit PlatformData(NativeAppWindow *window)
        : owner(window)
    {
    }

    NativeAppWindow *owner = nullptr;
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_compositor *compositor = nullptr;
    wl_subcompositor *subcompositor = nullptr;
    wl_surface *surface = nullptr;
    wl_webos_shell *webosShell = nullptr;
    wl_webos_shell_surface *webosShellSurface = nullptr;
    wl_webos_foreign *webosForeign = nullptr;
    wl_webos_exported *exported = nullptr;
    std::string windowId;
    int fullscreenRequestGeneration = 0;
    bool fullscreenConfirmationPending = false;
    int foregroundAttempts = 0;
    QMutex cropMutex;
    CropRegion pendingCrop;
    bool cropUpdateQueued = false;
    int cropOrigW = 0;
    int cropOrigH = 0;
    int cropSrcX = 0;
    int cropSrcY = 0;
    int cropSrcW = 0;
    int cropSrcH = 0;
    int cropDstX = 0;
    int cropDstY = 0;
    int cropDstW = 0;
    int cropDstH = 0;

    static void registryGlobal(
        void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
    static void registryRemove(void *, wl_registry *, uint32_t);
    static void exportedWindowIdAssigned(void *data, wl_webos_exported *, const char *windowId, uint32_t);
    static void shellStateChanged(void *data, wl_webos_shell_surface *, uint32_t state);
    static void shellPositionChanged(void *, wl_webos_shell_surface *, int32_t, int32_t);
    static void shellClose(void *data, wl_webos_shell_surface *);
    static void shellExposed(void *data, wl_webos_shell_surface *, wl_array *rectangles);
    static void shellStateAboutToChange(void *, wl_webos_shell_surface *, uint32_t);
    static void shellAddonStatusChanged(void *, wl_webos_shell_surface *, uint32_t);
    static uint8_t *overlayAcquire(void *data, int x, int y, int width, int height, int *stride, void **buffer);
    static void overlayPresent(void *data, void *buffer, bool visible);
    static void exportedCrop(void *data, int origW, int origH, int srcX, int srcY, int srcW, int srcH, int dstX,
        int dstY, int dstW, int dstH);

    static const wl_registry_listener registryListener;
    static const wl_webos_exported_listener exportedListener;
    static const wl_webos_shell_surface_listener shellSurfaceListener;
};

NativeAppWindow::NativeAppWindow(const QString& appId, QWindow *parent)
    : QQuickView(parent)
    , m_appId(appId)
    , m_platform(std::make_unique<PlatformData>(this))
{
    setColor(Qt::transparent);
    setResizeMode(QQuickView::SizeRootObjectToView);
    setFlags(Qt::FramelessWindowHint | Qt::Window);
    setTitle(QStringLiteral("Spool"));
    resize(1920, 1080);
    // Note: cursor suppression on webOS happens in main.cpp via
    // XCURSOR_PATH=/dev/null. Setting Qt::BlankCursor here would make Qt
    // call wl_pointer_set_cursor(NULL), which the LSM honours as "hide
    // cursor" — that hides the LG remote pointer too. Forcing the cursor
    // theme load to fail makes Qt early-return from updateCursor and
    // never touch the cursor, leaving the LSM pointer untouched.
    starfish_overlay_set_callbacks(&PlatformData::overlayAcquire, &PlatformData::overlayPresent, m_platform.get());
    starfish_exported_set_crop_cb(&PlatformData::exportedCrop, m_platform.get());
}

NativeAppWindow::~NativeAppWindow()
{
    starfish_exported_set_crop_cb(nullptr, nullptr);
    starfish_overlay_set_callbacks(nullptr, nullptr, nullptr);
    releasePlatformSurface();
    if (m_platform->webosShell)
        wl_webos_shell_destroy(m_platform->webosShell);
    if (m_platform->webosForeign)
        wl_webos_foreign_destroy(m_platform->webosForeign);
    if (m_platform->registry)
        wl_registry_destroy(m_platform->registry);
}

void NativeAppWindow::setVideoUnderlayActive(bool)
{
    // Nothing to get out of the way of here.
}

bool NativeAppWindow::prepareForUiSurface()
{
    showFullScreen();
    requestActivate();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    // Do not block app startup for seconds waiting on the shell surface.
    // The login UI can appear first; playback still performs a stricter wait
    // later when the video export surface is actually needed.
    if (ensureShellSurface())
        return true;

    return handle() != nullptr;
}

void NativeAppWindow::bringToFront()
{
    showFullScreen();
    requestActivate();
    requestUpdate();
    if (ensureShellSurface()) {
        m_platform->foregroundAttempts = 0;
        requestPlatformFullscreen();
        return;
    }

    // A relaunch can reach us before the compositor has handed the window its
    // surface back, and nothing else re-issues the request: the app then sits
    // behind whatever is in front of it, with the remote's keys going there,
    // until something happens to ask again. Keep asking until there is a
    // surface to ask with.
    constexpr int kForegroundRetryMs = 100;
    constexpr int kMaxForegroundAttempts = 20;
    if (m_platform->foregroundAttempts >= kMaxForegroundAttempts) {
        qWarning() << "webOS shell: no surface to bring to the front after"
                   << kMaxForegroundAttempts * kForegroundRetryMs << "ms";
        m_platform->foregroundAttempts = 0;
        return;
    }
    ++m_platform->foregroundAttempts;
    QTimer::singleShot(kForegroundRetryMs, this, [this]() { bringToFront(); });
}

void NativeAppWindow::requestPlatformFullscreen()
{
    if (!m_platform->webosShellSurface)
        return;

    const quint64 generation = ++m_platform->fullscreenRequestGeneration;
    m_platform->fullscreenConfirmationPending = true;
    wl_webos_shell_surface_set_state(m_platform->webosShellSurface, WL_WEBOS_SHELL_SURFACE_STATE_FULLSCREEN);
    applyPlatformKeyMask();
    if (m_platform->surface)
        wl_surface_commit(m_platform->surface);
    if (m_platform->display)
        wl_display_flush(m_platform->display);

    QTimer::singleShot(250, this, [this, generation]() {
        if (!m_platform->fullscreenConfirmationPending || generation != m_platform->fullscreenRequestGeneration)
            return;
        qInfo() << "webOS shell: fullscreen not yet confirmed; retrying";
        requestPlatformFullscreen();
        QTimer::singleShot(500, this, [this, generation]() {
            if (m_platform->fullscreenConfirmationPending && generation == m_platform->fullscreenRequestGeneration)
                qWarning() << "webOS shell: fullscreen request remained unconfirmed";
        });
    });
}

void NativeAppWindow::releasePlatformSurface()
{
    ++m_platform->fullscreenRequestGeneration;
    m_platform->fullscreenConfirmationPending = false;
    if (m_platform->exported) {
        wl_webos_exported_destroy(m_platform->exported);
        m_platform->exported = nullptr;
    }
    if (m_platform->webosShellSurface) {
        wl_webos_shell_surface_destroy(m_platform->webosShellSurface);
        m_platform->webosShellSurface = nullptr;
    }
    m_platform->surface = nullptr;
    m_platform->windowId.clear();
}

void NativeAppWindow::toggleFullScreen()
{
    if (!fullScreen())
        showFullScreen();
    emit fullScreenChanged();
}

// A television has no system bars to clear, so the window is always the whole
// screen and there is nothing to switch.
void NativeAppWindow::setImmersive(bool immersive)
{
    m_immersive = immersive;
}

void NativeAppWindow::exitToLauncher()
{
    close();
}

bool NativeAppWindow::prepareForPlaybackSurface()
{
    if (!prepareForUiSurface())
        return false;
    constexpr int timeoutMs = 5000;
    constexpr int stepMs = 25;
    for (int waited = 0; waited < timeoutMs; waited += stepMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, stepMs);
        if (ensureVideoSurface() && !m_platform->windowId.empty())
            break;
        QThread::msleep(stepMs);
    }
    return !m_platform->windowId.empty();
}

QString NativeAppWindow::windowId() const
{
    return QString::fromStdString(m_platform->windowId);
}

void NativeAppWindow::exposeEvent(QExposeEvent *event)
{
    QQuickView::exposeEvent(event);
    if (isExposed())
        ensureShellSurface();
}

void NativeAppWindow::resizeEvent(QResizeEvent *event)
{
    QQuickView::resizeEvent(event);
    if (!m_platform->windowId.empty())
        updateCropRegion();
}

bool NativeAppWindow::ensureVideoSurface()
{
    if (m_platform->exported || !handle())
        return true;
    if (!ensureShellSurface() || !m_platform->webosForeign || !m_platform->surface)
        return false;

    m_platform->exported = wl_webos_foreign_export_element(
        m_platform->webosForeign, m_platform->surface, WL_WEBOS_FOREIGN_WEBOS_EXPORTED_TYPE_VIDEO_OBJECT);
    if (!m_platform->exported)
        return false;
    wl_webos_exported_add_listener(m_platform->exported, &PlatformData::exportedListener, m_platform.get());
    wl_surface_commit(m_platform->surface);
    wl_display_roundtrip(m_platform->display);
    updateCropRegion();
    return true;
}

bool NativeAppWindow::ensureShellSurface()
{
    if (m_platform->webosShellSurface && m_platform->surface)
        return true;
    if (!handle())
        return false;

    auto *native = QGuiApplication::platformNativeInterface();
    if (!native)
        return false;
    m_platform->display = static_cast<wl_display *>(native->nativeResourceForIntegration(QByteArrayLiteral("display")));
    m_platform->surface
        = static_cast<wl_surface *>(native->nativeResourceForWindow(QByteArrayLiteral("surface"), this));
    if (!m_platform->display || !m_platform->surface || !bindPlatformGlobals())
        return false;
    if (!m_platform->webosShell)
        return false;

    m_platform->webosShellSurface = wl_webos_shell_get_shell_surface(m_platform->webosShell, m_platform->surface);
    if (!m_platform->webosShellSurface)
        return false;
    wl_webos_shell_surface_add_listener(
        m_platform->webosShellSurface, &PlatformData::shellSurfaceListener, m_platform.get());
    wl_webos_shell_surface_set_property(m_platform->webosShellSurface, "_WEBOS_ACCESS_POLICY_KEYS_GUIDE", "true");
    wl_webos_shell_surface_set_property(m_platform->webosShellSurface, "_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");
    wl_webos_shell_surface_set_property(m_platform->webosShellSurface, "appId", m_appId.toUtf8().constData());
    wl_webos_shell_surface_set_property(
        m_platform->webosShellSurface, "displayAffinity", std::getenv("DISPLAY_ID") ? std::getenv("DISPLAY_ID") : "0");
    wl_webos_shell_surface_set_state(m_platform->webosShellSurface, WL_WEBOS_SHELL_SURFACE_STATE_FULLSCREEN);
    applyPlatformKeyMask();
    wl_surface_commit(m_platform->surface);
    wl_display_roundtrip(m_platform->display);
    return true;
}

void NativeAppWindow::applyPlatformKeyMask()
{
    if (!m_platform->webosShellSurface)
        return;
    constexpr uint32_t keyMask = WL_WEBOS_SHELL_SURFACE_WEBOS_KEY_DEFAULT | WL_WEBOS_SHELL_SURFACE_WEBOS_KEY_BACK;
    wl_webos_shell_surface_set_key_mask(m_platform->webosShellSurface, keyMask);
    qInfo().nospace() << "webOS shell key mask applied mask=0x" << QString::number(keyMask, 16);
}

bool NativeAppWindow::bindPlatformGlobals()
{
    if (m_platform->webosForeign && m_platform->compositor)
        return true;
    m_platform->registry = wl_display_get_registry(m_platform->display);
    if (!m_platform->registry)
        return false;
    wl_registry_add_listener(m_platform->registry, &PlatformData::registryListener, m_platform.get());
    wl_display_roundtrip(m_platform->display);
    wl_display_roundtrip(m_platform->display);
    return m_platform->compositor && m_platform->subcompositor && m_platform->webosForeign && m_platform->webosShell;
}

void NativeAppWindow::updateCropRegion()
{
    if (!m_platform->exported || !m_platform->compositor)
        return;
    const int origW = m_platform->cropOrigW > 0 ? m_platform->cropOrigW : width();
    const int origH = m_platform->cropOrigH > 0 ? m_platform->cropOrigH : height();
    const int srcX = m_platform->cropSrcW > 0 ? m_platform->cropSrcX : 0;
    const int srcY = m_platform->cropSrcH > 0 ? m_platform->cropSrcY : 0;
    const int srcW = m_platform->cropSrcW > 0 ? m_platform->cropSrcW : origW;
    const int srcH = m_platform->cropSrcH > 0 ? m_platform->cropSrcH : origH;
    const int dstX = m_platform->cropDstW > 0 ? m_platform->cropDstX : 0;
    const int dstY = m_platform->cropDstH > 0 ? m_platform->cropDstY : 0;
    const int dstW = m_platform->cropDstW > 0 ? m_platform->cropDstW : width();
    const int dstH = m_platform->cropDstH > 0 ? m_platform->cropDstH : height();
    wl_region *orig = wl_compositor_create_region(m_platform->compositor);
    wl_region *src = wl_compositor_create_region(m_platform->compositor);
    wl_region *dst = wl_compositor_create_region(m_platform->compositor);
    if (!orig || !src || !dst)
        return;
    wl_region_add(orig, 0, 0, origW, origH);
    wl_region_add(src, srcX, srcY, srcW, srcH);
    wl_region_add(dst, dstX, dstY, dstW, dstH);
    wl_webos_exported_set_crop_region(m_platform->exported, orig, src, dst);
    wl_region_destroy(dst);
    wl_region_destroy(src);
    wl_region_destroy(orig);
    wl_display_flush(m_platform->display);
}

void NativeAppWindow::setVideoCrop(
    int origW, int origH, int srcX, int srcY, int srcW, int srcH, int dstX, int dstY, int dstW, int dstH)
{
    if (origW <= 0 || origH <= 0 || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0)
        return;
    const bool changed = m_platform->cropOrigW != origW || m_platform->cropOrigH != origH
        || m_platform->cropSrcX != srcX || m_platform->cropSrcY != srcY || m_platform->cropSrcW != srcW
        || m_platform->cropSrcH != srcH || m_platform->cropDstX != dstX || m_platform->cropDstY != dstY
        || m_platform->cropDstW != dstW || m_platform->cropDstH != dstH;
    if (!changed)
        return;
    m_platform->cropOrigW = origW;
    m_platform->cropOrigH = origH;
    m_platform->cropSrcX = srcX;
    m_platform->cropSrcY = srcY;
    m_platform->cropSrcW = srcW;
    m_platform->cropSrcH = srcH;
    m_platform->cropDstX = dstX;
    m_platform->cropDstY = dstY;
    m_platform->cropDstW = dstW;
    m_platform->cropDstH = dstH;
    updateCropRegion();
}

void NativeAppWindow::scheduleVideoCrop(
    int origW, int origH, int srcX, int srcY, int srcW, int srcH, int dstX, int dstY, int dstW, int dstH)
{
    if (origW <= 0 || origH <= 0 || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0)
        return;
    if (QThread::currentThread() == thread()) {
        setVideoCrop(origW, origH, srcX, srcY, srcW, srcH, dstX, dstY, dstW, dstH);
        return;
    }
    bool shouldQueue = false;
    {
        QMutexLocker locker(&m_platform->cropMutex);
        m_platform->pendingCrop = { origW, origH, srcX, srcY, srcW, srcH, dstX, dstY, dstW, dstH, true };
        if (!m_platform->cropUpdateQueued) {
            m_platform->cropUpdateQueued = true;
            shouldQueue = true;
        }
    }
    if (shouldQueue)
        QMetaObject::invokeMethod(this, [this]() { publishPendingVideoCrop(); }, Qt::QueuedConnection);
}

void NativeAppWindow::publishPendingVideoCrop()
{
    PlatformData::CropRegion crop;
    {
        QMutexLocker locker(&m_platform->cropMutex);
        crop = m_platform->pendingCrop;
        m_platform->pendingCrop = {};
        m_platform->cropUpdateQueued = false;
    }
    if (crop.valid) {
        setVideoCrop(crop.origW, crop.origH, crop.srcX, crop.srcY, crop.srcW, crop.srcH, crop.dstX, crop.dstY,
            crop.dstW, crop.dstH);
    }
}

void NativeAppWindow::handlePlatformSurfaceCreated()
{
    ensureShellSurface();
}

void NativeAppWindow::handlePlatformSurfaceAboutToBeDestroyed()
{
    releasePlatformSurface();
}

void NativeAppWindow::PlatformData::registryGlobal(
    void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    auto *platform = static_cast<PlatformData *>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        const uint32_t bindVersion = std::min(version, uint32_t { 3 });
        platform->compositor
            = static_cast<wl_compositor *>(wl_registry_bind(registry, name, &wl_compositor_interface, bindVersion));
    } else if (std::strcmp(interface, wl_subcompositor_interface.name) == 0) {
        platform->subcompositor
            = static_cast<wl_subcompositor *>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    } else if (std::strcmp(interface, wl_webos_shell_interface.name) == 0) {
        const uint32_t bindVersion = std::min(version, uint32_t { 2 });
        platform->webosShell
            = static_cast<wl_webos_shell *>(wl_registry_bind(registry, name, &wl_webos_shell_interface, bindVersion));
    } else if (std::strcmp(interface, wl_webos_foreign_interface.name) == 0) {
        const uint32_t bindVersion = std::min(version, uint32_t { 2 });
        platform->webosForeign = static_cast<wl_webos_foreign *>(
            wl_registry_bind(registry, name, &wl_webos_foreign_interface, bindVersion));
    }
}

void NativeAppWindow::PlatformData::registryRemove(void *, wl_registry *, uint32_t) { }

void NativeAppWindow::PlatformData::exportedWindowIdAssigned(
    void *data, wl_webos_exported *, const char *windowId, uint32_t)
{
    auto *platform = static_cast<PlatformData *>(data);
    platform->windowId = windowId ? windowId : "";
}

void NativeAppWindow::PlatformData::shellStateChanged(void *data, wl_webos_shell_surface *, uint32_t state)
{
    auto *platform = static_cast<PlatformData *>(data);
    if (!platform || !platform->owner)
        return;
    QMetaObject::invokeMethod(
        platform->owner,
        [platform, state]() {
            qInfo() << "webOS shell state changed:" << state;
            if (state == WL_WEBOS_SHELL_SURFACE_STATE_FULLSCREEN)
                platform->fullscreenConfirmationPending = false;
            emit platform->owner->platformSurfaceStateChanged(static_cast<int>(state));
        },
        Qt::QueuedConnection);
}

void NativeAppWindow::PlatformData::shellPositionChanged(void *, wl_webos_shell_surface *, int32_t, int32_t) { }

void NativeAppWindow::PlatformData::shellClose(void *data, wl_webos_shell_surface *)
{
    auto *platform = static_cast<PlatformData *>(data);
    if (platform && platform->owner) {
        QMetaObject::invokeMethod(
            platform->owner, [platform]() { emit platform->owner->platformCloseRequested(); }, Qt::QueuedConnection);
    }
}

void NativeAppWindow::PlatformData::shellExposed(void *data, wl_webos_shell_surface *, wl_array *rectangles)
{
    auto *platform = static_cast<PlatformData *>(data);
    if (!platform || !platform->owner)
        return;
    bool exposed = false;
    if (rectangles && rectangles->data && rectangles->size >= sizeof(int32_t)) {
        const auto *values = static_cast<const int32_t *>(rectangles->data);
        exposed = values[0] >= 0;
    }
    QMetaObject::invokeMethod(
        platform->owner,
        [platform, exposed]() {
            if (exposed)
                platform->fullscreenConfirmationPending = false;
            qInfo() << "webOS shell exposed:" << exposed;
            emit platform->owner->platformSurfaceExposed(exposed);
        },
        Qt::QueuedConnection);
}

void NativeAppWindow::PlatformData::shellStateAboutToChange(void *, wl_webos_shell_surface *, uint32_t) { }

void NativeAppWindow::PlatformData::shellAddonStatusChanged(void *, wl_webos_shell_surface *, uint32_t) { }

uint8_t *NativeAppWindow::PlatformData::overlayAcquire(
    void *data, int x, int y, int width, int height, int *stride, void **buffer)
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

void NativeAppWindow::PlatformData::overlayPresent(void *data, void *buffer, bool visible)
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

void NativeAppWindow::PlatformData::exportedCrop(
    void *data, int origW, int origH, int srcX, int srcY, int srcW, int srcH, int dstX, int dstY, int dstW, int dstH)
{
    auto *platform = static_cast<PlatformData *>(data);
    if (platform && platform->owner) {
        platform->owner->scheduleVideoCrop(origW, origH, srcX, srcY, srcW, srcH, dstX, dstY, dstW, dstH);
    }
}

const wl_registry_listener NativeAppWindow::PlatformData::registryListener = {
    &NativeAppWindow::PlatformData::registryGlobal,
    &NativeAppWindow::PlatformData::registryRemove,
};

const wl_webos_exported_listener NativeAppWindow::PlatformData::exportedListener = {
    &NativeAppWindow::PlatformData::exportedWindowIdAssigned,
};

const wl_webos_shell_surface_listener NativeAppWindow::PlatformData::shellSurfaceListener = {
    &NativeAppWindow::PlatformData::shellStateChanged,
    &NativeAppWindow::PlatformData::shellPositionChanged,
    &NativeAppWindow::PlatformData::shellClose,
    &NativeAppWindow::PlatformData::shellExposed,
    &NativeAppWindow::PlatformData::shellStateAboutToChange,
    &NativeAppWindow::PlatformData::shellAddonStatusChanged,
};

} // namespace Spool
