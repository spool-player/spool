#pragma once

#include <QByteArray>
#include <QMutex>
#include <QPointer>
#include <QtQmlIntegration/qqmlintegration.h>

#include <atomic>
#include <memory>

// JELLYFIN_MPV_ITEM_RHI comes from a header CMake generates, which knows the
// platform before any Qt header does -- and so does moc, which would otherwise
// disagree with the compiler about which class this derives from. A generated
// header rather than a compile definition so that changing the answer is a
// file change both of them depend on; see the comment in CMakeLists.txt.
#include "MpvVideoItemBase.h"
#if !defined(JELLYFIN_MPV_ITEM_RHI)
#error "MpvVideoItemBase.h was not found on the include path"
#endif

#if JELLYFIN_MPV_ITEM_RHI
#include <QQuickRhiItem>
#define JELLYFIN_MPV_ITEM_BASE QQuickRhiItem
#else
#include <QQuickFramebufferObject>
#define JELLYFIN_MPV_ITEM_BASE QQuickFramebufferObject
#endif

struct mpv_handle;
struct mpv_render_context;

namespace JellyfinNative {

// The scene-graph item that hosts libmpv's render API. PlayerController hands
// us an mpv_handle via setMpvHandle(); the render thread then creates an
// mpv_render_context bound to Qt's graphics device and renders each frame into
// the item's own target. Used by desktop playback and the webOS software
// decoder path, where a standalone mpv Wayland window cannot be embedded.
//
// Desktop renders through the RHI, so the scene graph can be Vulkan and the
// target can be a floating-point image an HDR swapchain will accept. Android
// and webOS keep the framebuffer item: neither has an HDR swapchain to reach,
// and neither can be tested from here.
class MpvVideoItem : public JELLYFIN_MPV_ITEM_BASE {
    Q_OBJECT
    QML_NAMED_ELEMENT(MpvVideoItem)

public:
    explicit MpvVideoItem(QQuickItem *parent = nullptr);
    ~MpvVideoItem() override;

    // Schedule render-context creation on Qt's scene-graph thread.
    void setMpvHandle(mpv_handle *handle);

    // GUI-thread selection for the next handle attachment, not a live switch.
    // webOS accepts gpu/gpu-next; empty or auto keeps its gpu default.
    // Other platforms retain their existing backend selection.
    void setRenderBackend(const QByteArray& backend);

    // Wait until the scheduled render-context handoff has completed. The
    // player must make this item visible before calling this so Qt will render
    // it; media loading must not start until this returns true.
    bool waitForRenderContext(int timeoutMs = 5000);

    // Release the render context on Qt's render thread and wait for that short
    // handoff before PlayerController destroys the underlying mpv core.
    bool releaseMpvHandle(int timeoutMs = 5000);

    static MpvVideoItem *instance();

#if JELLYFIN_MPV_ITEM_RHI
    QQuickRhiItemRenderer *createRenderer() override;
#else
    Renderer *createRenderer() const override;
#endif

    // Renderer-side: atomically read the latest handle update and clear the
    // dirty flag. `dirty` is true only on the first sync after setMpvHandle().
    struct HandleSnapshot {
        mpv_handle *handle;
        bool dirty;
        QPointer<QObject> releaseWaiter;
        std::shared_ptr<std::atomic_bool> releaseCompleted;
        std::shared_ptr<std::atomic_bool> attachCompleted;
        QByteArray renderBackend;
    };
    HandleSnapshot takePendingHandle();

    // Published for lifecycle diagnostics. Creation and destruction happen
    // exclusively in Renderer::render(), with the original GL context current.
    std::atomic<mpv_render_context *> m_renderCtxAtomic { nullptr };

signals:
    void renderError(const QString& message);
    void renderContextHandoffCompleted();

private:
    static MpvVideoItem *s_instance;

    QMutex m_handleMutex;
    mpv_handle *m_pendingHandle = nullptr;
    QByteArray m_renderBackend;
    QByteArray m_pendingRenderBackend;
    bool m_handleDirty = false;
    QPointer<QObject> m_releaseWaiter;
    std::shared_ptr<std::atomic_bool> m_releaseCompleted;
    std::shared_ptr<std::atomic_bool> m_attachCompleted;
};

} // namespace JellyfinNative
