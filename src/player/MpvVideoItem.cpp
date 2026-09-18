#include "MpvVideoItem.h"

#include <QColor>
#include <QEventLoop>
#include <QMetaObject>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>
#include <QOpenGLFunctions>
#include <QPointer>
#include <QQuickWindow>
#include <QTimer>
#include <QtDebug>

#if JELLYFIN_MPV_ITEM_RHI
#include <QSGRendererInterface>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
// Whether this build can actually compile Vulkan, asked of both halves of what
// that needs. Qt having the feature is not enough -- the Windows Qt has
// QVulkanInstance while the Vulkan headers themselves are absent -- and
// QT_CONFIG is no help either, since it reads a feature macro that a mixed set
// of include paths can answer for a different Qt than the one supplying the
// headers.
#if __has_include(<QVulkanInstance>) && __has_include(<vulkan/vulkan.h>)
#define JELLYFIN_MPV_ITEM_VULKAN 1
#include <QVersionNumber>
#include <QVulkanInstance>
#include <vulkan/vulkan.h>
#endif
#if defined(Q_OS_WIN) && __has_include(<d3d11.h>)
#define JELLYFIN_MPV_ITEM_D3D11 1
#include <d3d11.h>
#endif
#endif

#include <utility>
#include <vector>

extern "C" {
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
#if defined(JELLYFIN_MPV_ITEM_VULKAN)
#include <mpv/render_vk.h>
#endif
#if defined(JELLYFIN_MPV_ITEM_D3D11)
#include <mpv/render_d3d11.h>
#endif
}

namespace JellyfinNative {

namespace {

    void *getProcAddressGl(void *, const char *name)
    {
        QOpenGLContext *gl = QOpenGLContext::currentContext();
        if (!gl)
            return nullptr;
        return reinterpret_cast<void *>(gl->getProcAddress(QByteArray(name)));
    }

    // Render-thread ownership shared by both Qt item backends. GPU work stays
    // in the renderers, including the external-command boundary around a handoff.
    class RenderLifecycle final {
    public:
        explicit RenderLifecycle(MpvVideoItem *item)
            : m_item(item)
        {
        }

        ~RenderLifecycle()
        {
            disconnectWindow();
            releaseContext();
        }

        RenderLifecycle(const RenderLifecycle&) = delete;
        RenderLifecycle& operator=(const RenderLifecycle&) = delete;

        MpvVideoItem *item() const
        {
            return m_item;
        }
        QQuickWindow *window() const
        {
            return m_window;
        }
        mpv_render_context *context() const
        {
            return m_item ? m_item->m_renderCtxAtomic.load() : nullptr;
        }

        void synchronize(MpvVideoItem *item)
        {
            setWindow(item->window());
            m_item = item;
            auto pending = item->takePendingHandle();
            if (pending.dirty)
                m_pending = std::move(pending);
        }

        bool hasPendingHandle() const
        {
            return m_pending.dirty;
        }
        mpv_handle *nextHandle() const
        {
            return m_pending.handle;
        }
        const QByteArray& nextRenderBackend() const
        {
            return m_pending.renderBackend;
        }

        // Call only after backend creation/destruction and external commands
        // have finished: the GUI thread may destroy the mpv core when woken.
        void completeHandoff()
        {
            m_pending.handle = nullptr;
            m_pending.dirty = false;
            if (m_pending.releaseCompleted) {
                m_pending.releaseCompleted->store(true);
                if (m_pending.releaseWaiter)
                    QMetaObject::invokeMethod(m_pending.releaseWaiter, "quit", Qt::QueuedConnection);
                m_pending.releaseWaiter = nullptr;
                m_pending.releaseCompleted.reset();
            }
            if (m_pending.attachCompleted) {
                m_pending.attachCompleted->store(true);
                m_pending.attachCompleted.reset();
                if (m_item)
                    QMetaObject::invokeMethod(m_item, "renderContextHandoffCompleted", Qt::QueuedConnection);
            }
        }

        void publishContext(mpv_render_context *context)
        {
            mpv_render_context_set_update_callback(context, &RenderLifecycle::onMpvUpdate, m_item);
            m_item->m_renderCtxAtomic.store(context);
        }

        void releaseContext()
        {
            resetFrames();
            if (!m_item)
                return;
            if (auto *context = m_item->m_renderCtxAtomic.exchange(nullptr)) {
                mpv_render_context_set_update_callback(context, nullptr, nullptr);
                mpv_render_context_free(context);
            }
        }

        void invalidateFrame()
        {
            m_hasRenderedFrame = false;
        }

        void resetFrames()
        {
            m_hasRenderedFrame = false;
            m_hasRenderedVideoFrame = false;
            m_swapPending = false;
            m_firstVideoFrameSwapPending = false;
        }

        bool needsFrame(uint64_t updateFlags) const
        {
            return !m_hasRenderedFrame || (updateFlags & MPV_RENDER_UPDATE_FRAME);
        }

        void frameRendered(uint64_t updateFlags)
        {
            m_hasRenderedFrame = true;
            m_swapPending = true;
            if (!m_hasRenderedVideoFrame && (updateFlags & MPV_RENDER_UPDATE_FRAME)) {
                m_hasRenderedVideoFrame = true;
                m_firstVideoFrameSwapPending = true;
                qInfo() << "player: first video frame rendered";
            }
        }

        // RHI disconnects before destroying its GL framebuffer; context
        // destruction still follows that backend-specific cleanup.
        void disconnectWindow()
        {
            QObject::disconnect(m_frameSwappedConnection);
        }

    private:
        void setWindow(QQuickWindow *window)
        {
            if (m_window == window)
                return;

            disconnectWindow();
            m_window = window;
            if (!m_window)
                return;

            m_frameSwappedConnection = QObject::connect(
                m_window, &QQuickWindow::frameSwapped, m_window,
                [this] {
                    if (!m_item || !m_swapPending)
                        return;

                    m_swapPending = false;
                    if (auto *ctx = context()) {
                        mpv_render_context_report_swap(ctx);
                        if (m_firstVideoFrameSwapPending) {
                            m_firstVideoFrameSwapPending = false;
                            qInfo() << "player: first video frame swapped";
                        }
                    }
                },
                Qt::DirectConnection);
        }

        static void onMpvUpdate(void *ctx)
        {
            auto *item = static_cast<MpvVideoItem *>(ctx);
            QMetaObject::invokeMethod(item, "update", Qt::QueuedConnection);
        }

        QMetaObject::Connection m_frameSwappedConnection;
        MpvVideoItem *m_item = nullptr;
        QQuickWindow *m_window = nullptr;
        MpvVideoItem::HandleSnapshot m_pending {};
        bool m_hasRenderedFrame = false;
        bool m_hasRenderedVideoFrame = false;
        bool m_swapPending = false;
        bool m_firstVideoFrameSwapPending = false;
    };

#if !JELLYFIN_MPV_ITEM_RHI

    class MpvFboRenderer final : public QQuickFramebufferObject::Renderer {
    public:
        explicit MpvFboRenderer(MpvVideoItem *item)
            : m_lifecycle(item)
        {
        }

        QOpenGLFramebufferObject *createFramebufferObject(const QSize& size) override
        {
            m_lifecycle.resetFrames();
            QOpenGLFramebufferObjectFormat fmt;
            fmt.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
            return new QOpenGLFramebufferObject(size, fmt);
        }

        void synchronize(QQuickFramebufferObject *fbo) override
        {
            m_lifecycle.synchronize(static_cast<MpvVideoItem *>(fbo));
        }

        void render() override
        {
            if (!m_lifecycle.item())
                return;

            if (m_lifecycle.hasPendingHandle()) {
                m_lifecycle.releaseContext();
                if (auto *next = m_lifecycle.nextHandle())
                    createRenderContext(next);
                m_lifecycle.completeHandoff();
            }

            mpv_render_context *ctx = m_lifecycle.context();
            if (!ctx)
                return;

            const uint64_t updateFlags = mpv_render_context_update(ctx);
            if (!m_lifecycle.needsFrame(updateFlags))
                return;

            QOpenGLFramebufferObject *fbo = framebufferObject();
            if (!fbo)
                return;

            mpv_opengl_fbo mpfbo {};
            mpfbo.fbo = static_cast<int>(fbo->handle());
            mpfbo.w = fbo->width();
            mpfbo.h = fbo->height();
            mpfbo.internal_format = 0;

            mpv_render_param params[] = {
                { MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo },
                { MPV_RENDER_PARAM_INVALID, nullptr },
            };

            if (auto *window = m_lifecycle.window())
                window->beginExternalCommands();
            mpv_render_context_render(ctx, params);
            if (auto *window = m_lifecycle.window())
                window->endExternalCommands();
            m_lifecycle.frameRendered(updateFlags);
        }

    private:
        void createRenderContext(mpv_handle *next)
        {
            if (auto *gl = QOpenGLContext::currentContext()) {
                auto *functions = gl->functions();
                qInfo() << "player: OpenGL context" << gl->format()
                        << "vendor=" << reinterpret_cast<const char *>(functions->glGetString(GL_VENDOR))
                        << "renderer=" << reinterpret_cast<const char *>(functions->glGetString(GL_RENDERER))
                        << "version=" << reinterpret_cast<const char *>(functions->glGetString(GL_VERSION));
            }
            mpv_opengl_init_params glInit {};
            glInit.get_proc_address = &getProcAddressGl;
            // Deliberately do NOT set MPV_RENDER_PARAM_ADVANCED_CONTROL.
            //
            // advanced_control routes VOCTRL_PERFORMANCE_DATA (and SCREENSHOT)
            // through mp_dispatch_run on the render context's dispatch queue,
            // which is a *synchronous* call that blocks the caller until the
            // render thread next enters mpv_render_context_render. mpv's stats
            // overlay queries vo_passes (= VOCTRL_PERFORMANCE_DATA) on a
            // periodic lua timer while holding the core dispatch lock, so any
            // delay on the render thread (Wayland frame-callback throttling
            // under nixGL on this user's setup) stalls the core, freezing
            // playback. Without advanced_control the control falls through to
            // control_cb (null here) and returns VO_NOTIMPL immediately — stats
            // simply don't show GPU pass timings, and direct rendering / mpv
            // screenshots are disabled, neither of which we use.
#ifdef JELLYFIN_NATIVE_WEBOS
            const QByteArray& requestedBackend = m_lifecycle.nextRenderBackend();
            const char *backends[]
                = { requestedBackend.isEmpty() || requestedBackend == "auto" ? "gpu" : requestedBackend.constData() };
#else
            static constexpr const char *backends[] = { "gpu-next", "gpu" };
#endif

            mpv_render_context *newCtx = nullptr;
            int err = MPV_ERROR_UNSUPPORTED;
            for (const char *backend : backends) {
                mpv_render_param params[] = {
                    { MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL) },
                    { MPV_RENDER_PARAM_BACKEND, const_cast<char *>(backend) },
                    { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit },
                    { MPV_RENDER_PARAM_INVALID, nullptr },
                };

                newCtx = nullptr;
                err = mpv_render_context_create(&newCtx, next, params);
                if (err >= 0) {
                    qInfo() << "player: render backend" << backend;
                    break;
                }
                qWarning() << "player: render backend" << backend << "unavailable:" << mpv_error_string(err);
            }
            if (err < 0) {
                const QString message = QStringLiteral("Failed to initialize video rendering: %1")
                                            .arg(QString::fromUtf8(mpv_error_string(err)));
                qCritical() << "MpvVideoItem:" << message;
                const QPointer<MpvVideoItem> guardedItem(m_lifecycle.item());
                QMetaObject::invokeMethod(
                    m_lifecycle.item(),
                    [guardedItem, message]() {
                        if (guardedItem)
                            emit guardedItem->renderError(message);
                    },
                    Qt::QueuedConnection);
                return;
            }
            m_lifecycle.publishContext(newCtx);
        }

        RenderLifecycle m_lifecycle;
    };

#else // JELLYFIN_MPV_ITEM_RHI

    class MpvRhiRenderer final : public QQuickRhiItemRenderer {
    public:
        explicit MpvRhiRenderer(MpvVideoItem *item)
            : m_lifecycle(item)
        {
        }

        ~MpvRhiRenderer() override
        {
            m_lifecycle.disconnectWindow();
            destroyGlFramebuffer();
            // Release while backend-owned state (including Vulkan features)
            // is still alive, before member destruction reaches the lifecycle.
            m_lifecycle.releaseContext();
        }

    protected:
        void initialize(QRhiCommandBuffer *) override
        {
            // Qt may initialize on every synchronization, not just on resize.
            // Invalidate the cached frame only when its backing image changes.
            QRhiTexture *target = colorTexture();
            const quint64 object = target ? target->nativeTexture().object : 0;
            const QSize size = target ? target->pixelSize() : QSize();
            const int format = target ? int(target->format()) : -1;
            if (object != m_targetObject || size != m_targetSize || format != m_targetFormat) {
                m_targetObject = object;
                m_targetSize = size;
                m_targetFormat = format;
                m_lifecycle.invalidateFrame();
                destroyGlFramebuffer();
            }
        }

        void synchronize(QQuickRhiItem *rhiItem) override
        {
            m_lifecycle.synchronize(static_cast<MpvVideoItem *>(rhiItem));
        }

        void render(QRhiCommandBuffer *cb) override
        {
            if (!m_lifecycle.item())
                return;

            // libplacebo initialization and destruction also touch the D3D11
            // immediate context (destruction clears its state). Execute Qt's
            // pending commands first and invalidate its state cache afterwards.
            const bool d3d11Handoff = m_lifecycle.hasPendingHandle() && cb && rhi() && rhi()->backend() == QRhi::D3D11;
            if (d3d11Handoff)
                cb->beginExternal();
            if (m_lifecycle.hasPendingHandle()) {
                m_renderFailed = false;
                m_lifecycle.releaseContext();
                if (auto *next = m_lifecycle.nextHandle())
                    createRenderContext(next);
                if (d3d11Handoff)
                    cb->endExternal();
                m_lifecycle.completeHandoff();
            }

            mpv_render_context *ctx = m_lifecycle.context();
            if (!ctx || m_renderFailed)
                return;

            const uint64_t updateFlags = mpv_render_context_update(ctx);
            if (!m_lifecycle.needsFrame(updateFlags))
                return;

            QRhiTexture *target = colorTexture();
            if (!target || !cb)
                return;

            // This texture is the item's own and it outlives the frame: Qt
            // allocates it once and reuses it until the item is resized, so
            // whatever mpv does not draw over stays on screen. The letterbox
            // bars are that region, and what was last written there is the
            // stats page the viewer just closed -- it sat in the bars until a
            // resize reallocated the texture. Clear the target first; mpv draws
            // the picture over it.
            //
            // Not on Vulkan: mpv is handed the image layout Qt is tracking, and
            // a pass of our own moves the image out from under that handover.
            if (rhi() && rhi()->backend() != QRhi::Vulkan) {
                if (QRhiRenderTarget *renderTarget = this->renderTarget()) {
                    cb->beginPass(renderTarget, QColor(Qt::black), { 1.0f, 0 });
                    cb->endPass();
                }
            }

            // D3D11 executes Qt's pending commands before mpv uses the shared
            // immediate context; endExternal() invalidates Qt's state cache.
            // Vulkan uses the render API's semaphore handover on the shared queue.
            cb->beginExternal();
            const bool drew = renderInto(ctx, target);
            cb->endExternal();
            if (!drew)
                return;

            m_lifecycle.frameRendered(updateFlags);
        }

    private:
        bool renderInto(mpv_render_context *ctx, QRhiTexture *target)
        {
            const QSize size = target->pixelSize();
            if (size.isEmpty())
                return false;

#if defined(JELLYFIN_MPV_ITEM_D3D11)
            if (m_d3d11) {
                const QRhiTexture::NativeTexture native = target->nativeTexture();
                if (!native.object)
                    return false;
                mpv_d3d11_texture texture {};
                texture.texture = reinterpret_cast<void *>(native.object);
                texture.w = size.width();
                texture.h = size.height();

                mpv_render_param params[] = {
                    { MPV_RENDER_PARAM_D3D11_TEXTURE, &texture },
                    { MPV_RENDER_PARAM_INVALID, nullptr },
                };
                return checkRenderResult(mpv_render_context_render(ctx, params));
            }
#endif
#if defined(JELLYFIN_MPV_ITEM_VULKAN)
            if (m_vulkan) {
                const QRhiTexture::NativeTexture native = target->nativeTexture();
                if (!native.object)
                    return false;
                mpv_vulkan_image image {};
                image.image = native.object;
                image.format = vulkanFormat(target);
                // Only what mpv is actually asked to do with it. Claiming usage
                // the image was not created with is how libplacebo ends up
                // advertising a capability the driver will refuse.
                image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                    | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                image.w = size.width();
                image.h = size.height();
                image.layout = native.layout;

                mpv_render_param params[] = {
                    { MPV_RENDER_PARAM_VULKAN_IMAGE, &image },
                    { MPV_RENDER_PARAM_INVALID, nullptr },
                };
                const int err = mpv_render_context_render(ctx, params);
                // The API updates layout after a successful handover even if
                // drawing failed; a failure before handover leaves it intact.
                target->setNativeLayout(image.layout);
                return checkRenderResult(err);
            }
#endif
            const GLuint texture = static_cast<GLuint>(target->nativeTexture().object);
            if (!texture || !ensureGlFramebuffer(texture, size))
                return false;

            mpv_opengl_fbo mpfbo {};
            mpfbo.fbo = static_cast<int>(m_glFbo);
            mpfbo.w = size.width();
            mpfbo.h = size.height();
            mpfbo.internal_format = 0;
            // A framebuffer object has OpenGL's bottom-left origin, and the
            // scene graph samples this texture with the RHI's top-left one.
            // The framebuffer item used to reconcile that itself; here mpv is
            // asked to write the image the way it will be read.
            int flipY = 1;

            mpv_render_param params[] = {
                { MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo },
                { MPV_RENDER_PARAM_FLIP_Y, &flipY },
                { MPV_RENDER_PARAM_INVALID, nullptr },
            };
            return checkRenderResult(mpv_render_context_render(ctx, params));
        }

        bool checkRenderResult(int err)
        {
            if (err >= 0)
                return true;
            // No VO exists yet before a file starts playing.
            if (err == MPV_ERROR_UNINITIALIZED)
                return false;
            m_renderFailed = true;
            const QString message
                = QStringLiteral("Video rendering failed on %1: %2")
                      .arg(QString::fromLatin1(graphicsApiName()), QString::fromUtf8(mpv_error_string(err)));
            qCritical() << "MpvVideoItem:" << message;
            const QPointer<MpvVideoItem> guardedItem(m_lifecycle.item());
            QMetaObject::invokeMethod(
                m_lifecycle.item(),
                [guardedItem, message]() {
                    if (guardedItem)
                        emit guardedItem->renderError(message);
                },
                Qt::QueuedConnection);
            return false;
        }

        bool ensureGlFramebuffer(GLuint texture, const QSize& size)
        {
            if (m_glFbo && m_glFboTexture == texture && m_glFboSize == size)
                return true;

            QOpenGLContext *gl = QOpenGLContext::currentContext();
            if (!gl)
                return false;
            destroyGlFramebuffer();

            QOpenGLFunctions *functions = gl->functions();
            functions->glGenFramebuffers(1, &m_glFbo);
            if (!m_glFbo)
                return false;
            GLint previous = 0;
            functions->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
            functions->glBindFramebuffer(GL_FRAMEBUFFER, m_glFbo);
            functions->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            const GLenum status = functions->glCheckFramebufferStatus(GL_FRAMEBUFFER);
            functions->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                qWarning() << "player: incomplete framebuffer over the scene graph texture" << status;
                destroyGlFramebuffer();
                return false;
            }
            m_glFboTexture = texture;
            m_glFboSize = size;
            return true;
        }

        void destroyGlFramebuffer()
        {
            if (!m_glFbo)
                return;
            if (QOpenGLContext *gl = QOpenGLContext::currentContext())
                gl->functions()->glDeleteFramebuffers(1, &m_glFbo);
            m_glFbo = 0;
            m_glFboTexture = 0;
            m_glFboSize = QSize();
        }

        void createRenderContext(mpv_handle *next)
        {
            std::vector<mpv_render_param> params;
            mpv_opengl_init_params glInit {};
            glInit.get_proc_address = &getProcAddressGl;
#if defined(JELLYFIN_MPV_ITEM_D3D11)
            mpv_d3d11_init_params d3d11Init {};
            m_d3d11 = false;
#endif
#if defined(JELLYFIN_MPV_ITEM_VULKAN)
            mpv_vulkan_init_params vkInit {};
            m_vulkan = false;
#endif
            QRhi *rhi = this->rhi();
            if (!rhi) {
                qCritical() << "MpvVideoItem: no RHI to render through";
                return;
            }

#if defined(JELLYFIN_MPV_ITEM_D3D11)
            if (rhi->backend() == QRhi::D3D11) {
                const auto *native = static_cast<const QRhiD3D11NativeHandles *>(rhi->nativeHandles());
                if (!native || !native->dev) {
                    qCritical() << "MpvVideoItem: the Direct3D 11 device is not available";
                    return;
                }
                d3d11Init.device = native->dev;
                m_d3d11 = true;
                params.push_back({ MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_D3D11) });
                params.push_back({ MPV_RENDER_PARAM_D3D11_INIT_PARAMS, &d3d11Init });
            }
#endif
#if defined(JELLYFIN_MPV_ITEM_VULKAN)
            if (params.empty() && rhi->backend() == QRhi::Vulkan) {
                const auto *native = static_cast<const QRhiVulkanNativeHandles *>(rhi->nativeHandles());
                if (!native || !native->inst || !native->physDev || !native->dev) {
                    qCritical() << "MpvVideoItem: the Vulkan device is not available";
                    return;
                }
                if (native->gfxQueueIdx != 0) {
                    // mpv takes the family's first queue and orders its frame
                    // against the caller's submissions on it. A different index
                    // would be a different queue and no ordering at all.
                    qCritical() << "MpvVideoItem: Qt is on queue index" << native->gfxQueueIdx
                                << "which mpv cannot share";
                    return;
                }
                vkInit.instance = native->inst->vkInstance();
                vkInit.get_instance_proc_addr
                    = reinterpret_cast<void *>(native->inst->getInstanceProcAddr("vkGetInstanceProcAddr"));
                vkInit.physical_device = native->physDev;
                vkInit.device = native->dev;
                vkInit.queue_family_index = native->gfxQueueFamilyIdx;
                vkInit.device_features = queryDeviceFeatures(native->inst, native->physDev);
                if (!vkInit.device_features) {
                    qCritical() << "MpvVideoItem: cannot describe Qt's enabled Vulkan features";
                    return;
                }
                m_vulkan = true;
                params.push_back({ MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_VULKAN) });
                params.push_back({ MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, &vkInit });
            }
#endif
            if (params.empty()) {
                if (rhi->backend() != QRhi::OpenGLES2) {
                    qCritical() << "MpvVideoItem: no mpv render backend for this graphics API";
                    return;
                }
                if (auto *gl = QOpenGLContext::currentContext()) {
                    auto *functions = gl->functions();
                    qInfo() << "player: OpenGL context" << gl->format()
                            << "vendor=" << reinterpret_cast<const char *>(functions->glGetString(GL_VENDOR))
                            << "renderer=" << reinterpret_cast<const char *>(functions->glGetString(GL_RENDERER))
                            << "version=" << reinterpret_cast<const char *>(functions->glGetString(GL_VERSION));
                }
                params.push_back({ MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL) });
                params.push_back({ MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit });
            }

            // Deliberately do NOT set MPV_RENDER_PARAM_ADVANCED_CONTROL.
            //
            // advanced_control routes VOCTRL_PERFORMANCE_DATA (and SCREENSHOT)
            // through mp_dispatch_run on the render context's dispatch queue,
            // which is a *synchronous* call that blocks the caller until the
            // render thread next enters mpv_render_context_render. mpv's stats
            // overlay queries vo_passes (= VOCTRL_PERFORMANCE_DATA) on a
            // periodic lua timer while holding the core dispatch lock, so any
            // delay on the render thread (Wayland frame-callback throttling
            // under nixGL on this user's setup) stalls the core, freezing
            // playback. Without advanced_control the control falls through to
            // control_cb (null here) and returns VO_NOTIMPL immediately -- stats
            // simply don't show GPU pass timings, and direct rendering / mpv
            // screenshots are disabled, neither of which we use.
            static constexpr const char *backends[] = { "gpu-next", "gpu" };

            mpv_render_context *newCtx = nullptr;
            int err = MPV_ERROR_UNSUPPORTED;
            for (const char *backend : backends) {
                // The legacy renderer implements only the OpenGL render API.
                if (backend == backends[1] && rhi->backend() != QRhi::OpenGLES2)
                    break;
                std::vector<mpv_render_param> attempt = params;
                attempt.push_back({ MPV_RENDER_PARAM_BACKEND, const_cast<char *>(backend) });
                attempt.push_back({ MPV_RENDER_PARAM_INVALID, nullptr });

                newCtx = nullptr;
                err = mpv_render_context_create(&newCtx, next, attempt.data());
                if (err >= 0) {
                    qInfo() << "player: render backend" << backend << "on" << graphicsApiName();
                    break;
                }
                qWarning() << "player: render backend" << backend << "unavailable:" << mpv_error_string(err);
            }
            if (err < 0) {
                const QString message = QStringLiteral("Failed to initialize video rendering: %1")
                                            .arg(QString::fromUtf8(mpv_error_string(err)));
                qCritical() << "MpvVideoItem:" << message;
                const QPointer<MpvVideoItem> guardedItem(m_lifecycle.item());
                QMetaObject::invokeMethod(
                    m_lifecycle.item(),
                    [guardedItem, message]() {
                        if (guardedItem)
                            emit guardedItem->renderError(message);
                    },
                    Qt::QueuedConnection);
                return;
            }
            m_lifecycle.publishContext(newCtx);
        }

#if defined(JELLYFIN_MPV_ITEM_VULKAN)
        // libplacebo checks the features it requires against what the caller
        // says the device was created with -- not against the device. Passing
        // nothing is read as "nothing is enabled", and the import is refused
        // for want of hostQueryReset even though Qt did enable it.
        //
        // Qt enables every feature the device reports as supported, less the
        // two robustness ones it turns off, so that is what this reports.
        const void *queryDeviceFeatures(QVulkanInstance *instance, VkPhysicalDevice physicalDevice)
        {
            auto getFeatures = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                instance->getInstanceProcAddr("vkGetPhysicalDeviceFeatures2"));
            if (!getFeatures)
                return nullptr;

            const auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                instance->getInstanceProcAddr("vkGetPhysicalDeviceProperties"));
            if (!getProperties)
                return nullptr;
            VkPhysicalDeviceProperties properties {};
            getProperties(physicalDevice, &properties);
            const QVersionNumber deviceVersion(
                VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion));
            const QVersionNumber apiVersion = qMin(instance->apiVersion(), deviceVersion);
            if (apiVersion < QVersionNumber(1, 2))
                return nullptr;

            m_vkFeatures = {};
            m_vkFeatures.features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            m_vkFeatures.v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
            m_vkFeatures.v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            m_vkFeatures.v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            m_vkFeatures.features.pNext = &m_vkFeatures.v11;
            m_vkFeatures.v11.pNext = &m_vkFeatures.v12;
            // Only ask for a struct the implementation understands; a newer
            // one in the chain is not something older drivers have to ignore.
            if (apiVersion >= QVersionNumber(1, 3))
                m_vkFeatures.v12.pNext = &m_vkFeatures.v13;
            getFeatures(physicalDevice, &m_vkFeatures.features);

            m_vkFeatures.features.features.robustBufferAccess = VK_FALSE;
            m_vkFeatures.v13.robustImageAccess = VK_FALSE;
            return &m_vkFeatures.features;
        }

        // Qt does not say which VkFormat it gave the texture, so this mirrors
        // the mapping its Vulkan backend uses for the formats the item offers.
        static int vulkanFormat(QRhiTexture *texture)
        {
            if (!texture)
                return VK_FORMAT_R8G8B8A8_UNORM;
            switch (texture->format()) {
            case QRhiTexture::RGBA16F:
                return VK_FORMAT_R16G16B16A16_SFLOAT;
            case QRhiTexture::RGBA32F:
                return VK_FORMAT_R32G32B32A32_SFLOAT;
            case QRhiTexture::RGB10A2:
                return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            default:
                break;
            }
            return texture->flags().testFlag(QRhiTexture::sRGB) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        }
#endif

        RenderLifecycle m_lifecycle;
        bool m_renderFailed = false;
        quint64 m_targetObject = 0;
        QSize m_targetSize;
        int m_targetFormat = -1;
        const char *graphicsApiName() const
        {
#if defined(JELLYFIN_MPV_ITEM_D3D11)
            if (m_d3d11)
                return "Direct3D 11";
#endif
            return m_vulkan ? "Vulkan" : "OpenGL";
        }

        bool m_vulkan = false;
        bool m_d3d11 = false;
#if defined(JELLYFIN_MPV_ITEM_VULKAN)
        // Kept alive because libplacebo is handed a pointer into it.
        struct VulkanFeatures {
            VkPhysicalDeviceFeatures2 features;
            VkPhysicalDeviceVulkan11Features v11;
            VkPhysicalDeviceVulkan12Features v12;
            VkPhysicalDeviceVulkan13Features v13;
        };
        VulkanFeatures m_vkFeatures {};
#endif
        GLuint m_glFbo = 0;
        GLuint m_glFboTexture = 0;
        QSize m_glFboSize;
    };
#endif // JELLYFIN_MPV_ITEM_RHI
} // namespace

MpvVideoItem *MpvVideoItem::s_instance = nullptr;

MpvVideoItem::MpvVideoItem(QQuickItem *parent)
    : JELLYFIN_MPV_ITEM_BASE(parent)
{
#if JELLYFIN_MPV_ITEM_RHI
    // A floating-point target is what an HDR swapchain can be handed, and it
    // costs little when the swapchain is SDR: the extra precision is discarded
    // once at the end rather than at every step before it.
    setColorBufferFormat(TextureFormat::RGBA16F);
    setAlphaBlending(false);
#endif
    if (s_instance)
        qWarning() << "MpvVideoItem: replacing existing singleton instance";
    s_instance = this;
}

MpvVideoItem::~MpvVideoItem()
{
    if (s_instance == this)
        s_instance = nullptr;
}

MpvVideoItem *MpvVideoItem::instance()
{
    return s_instance;
}

void MpvVideoItem::setMpvHandle(mpv_handle *handle)
{
    Q_ASSERT(handle);
    {
        QMutexLocker locker(&m_handleMutex);
        m_pendingHandle = handle;
        m_pendingRenderBackend = m_renderBackend;
        m_handleDirty = true;
        m_releaseWaiter = nullptr;
        m_releaseCompleted.reset();
        m_attachCompleted = std::make_shared<std::atomic_bool>(false);
    }
    update();
}

void MpvVideoItem::setRenderBackend(const QByteArray& backend)
{
    m_renderBackend = backend;
}

bool MpvVideoItem::waitForRenderContext(int timeoutMs)
{
    std::shared_ptr<std::atomic_bool> completed;
    {
        QMutexLocker locker(&m_handleMutex);
        completed = m_attachCompleted;
    }
    if (!completed)
        return m_renderCtxAtomic.load() != nullptr;

    QEventLoop waitLoop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &waitLoop, &QEventLoop::quit);
    QObject::connect(this, &MpvVideoItem::renderContextHandoffCompleted, &waitLoop, &QEventLoop::quit);
    timeout.start(timeoutMs);
    if (!completed->load())
        waitLoop.exec(QEventLoop::ExcludeUserInputEvents);
    const bool ready = completed->load() && m_renderCtxAtomic.load() != nullptr;
    if (!ready) {
        // The handoff runs on the render thread, so it only happens once the
        // scene graph draws this item. Report what kept it from being drawn.
        qWarning() << "player: render context handoff did not complete within" << timeoutMs
                   << "ms handedOff=" << completed->load() << "visible=" << isVisible() << "size=" << width() << "x"
                   << height() << "window=" << (window() != nullptr)
                   << "windowExposed=" << (window() && window()->isExposed());
    }
    return ready;
}

bool MpvVideoItem::releaseMpvHandle(int timeoutMs)
{
    QEventLoop releaseLoop;
    const auto completed = std::make_shared<std::atomic_bool>(false);
    {
        QMutexLocker locker(&m_handleMutex);
        const bool needsRenderHandoff = m_renderCtxAtomic.load() || m_pendingHandle;
        m_pendingHandle = nullptr;
        m_handleDirty = true;
        if (!needsRenderHandoff)
            return true;
        m_releaseWaiter = &releaseLoop;
        m_releaseCompleted = completed;
    }

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &releaseLoop, &QEventLoop::quit);
    timeout.start(timeoutMs);
    update();
    releaseLoop.exec(QEventLoop::ExcludeUserInputEvents);
    return completed->load();
}

#if JELLYFIN_MPV_ITEM_RHI
QQuickRhiItemRenderer *MpvVideoItem::createRenderer()
{
    return new MpvRhiRenderer(this);
}
#else
QQuickFramebufferObject::Renderer *MpvVideoItem::createRenderer() const
{
    return new MpvFboRenderer(const_cast<MpvVideoItem *>(this));
}
#endif

MpvVideoItem::HandleSnapshot MpvVideoItem::takePendingHandle()
{
    QMutexLocker locker(&m_handleMutex);
    HandleSnapshot snap { m_pendingHandle, m_handleDirty, m_releaseWaiter, m_releaseCompleted, m_attachCompleted,
        m_pendingRenderBackend };
    m_handleDirty = false;
    m_releaseWaiter = nullptr;
    m_releaseCompleted.reset();
    return snap;
}

} // namespace JellyfinNative
