#include "MpvVideoItem.h"
#if defined(SPOOL_APPLE_MOBILE)
#include "platform/apple/AppleMobileRuntime.h"
#endif

#include <QColor>
#include <QEventLoop>
#include <QMetaObject>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>
#include <QOpenGLFunctions>
#include <QPointer>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>
#include <QTimer>
#include <QtDebug>
#ifdef Q_OS_TVOS
#include <OpenGLES/ES3/gl.h>
#include <cstring>
#include <cstdio>
#include <dlfcn.h>
#endif

#if SPOOL_MPV_ITEM_RHI
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
#define SPOOL_MPV_ITEM_VULKAN 1
#include <QVersionNumber>
#include <QVulkanInstance>
#include <vulkan/vulkan.h>
#endif
#if defined(Q_OS_WIN) && __has_include(<d3d11.h>)
#define SPOOL_MPV_ITEM_D3D11 1
#include <d3d11.h>
#endif
#endif

#include <utility>
#include <vector>

extern "C" {
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
#if defined(SPOOL_MPV_ITEM_VULKAN)
#include <mpv/render_vk.h>
#endif
#if defined(SPOOL_MPV_ITEM_D3D11)
#include <mpv/render_d3d11.h>
#endif
}

namespace Spool {

namespace {
#ifdef Q_OS_TVOS
    thread_local int nativeDrawDiagnostics = 0;
    thread_local GLfloat nativeVertexData[12] {};
    void diagnosticBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
    {
        if (target == GL_ARRAY_BUFFER && data && size >= GLsizeiptr(sizeof(nativeVertexData)))
            std::memcpy(nativeVertexData, data, sizeof(nativeVertexData));
        ::glBufferData(target, size, data, usage);
    }
    void diagnosticDrawArrays(GLenum mode, GLint first, GLsizei count)
    {
        GLint viewport[4] {}, scissor[4] {}, fbo = 0, program = 0, vao = 0;
        ::glGetIntegerv(GL_VIEWPORT, viewport);
        if (viewport[2] >= 2000 && nativeDrawDiagnostics++ < 12) {
            ::glGetIntegerv(GL_SCISSOR_BOX, scissor);
            ::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
            ::glGetIntegerv(GL_CURRENT_PROGRAM, &program);
            ::glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
            GLint enabled = 0, stride = 0, buffer = 0;
            ::glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
            ::glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
            ::glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
            std::fprintf(stderr,
                "native draw: vp=%d,%d,%d,%d scissor=%d,%d,%d,%d fbo=%d program=%d vao=%d attrib0=%d stride=%d buffer=%d vertices=%g,%g,%g,%g;%g,%g,%g,%g;%g,%g,%g,%g error=%u\n",
                viewport[0], viewport[1], viewport[2], viewport[3], scissor[0], scissor[1], scissor[2], scissor[3],
                fbo, program, vao, enabled, stride, buffer, double(nativeVertexData[0]), double(nativeVertexData[1]),
                double(nativeVertexData[2]), double(nativeVertexData[3]), double(nativeVertexData[4]), double(nativeVertexData[5]),
                double(nativeVertexData[6]), double(nativeVertexData[7]), double(nativeVertexData[8]), double(nativeVertexData[9]),
                double(nativeVertexData[10]), double(nativeVertexData[11]), unsigned(::glGetError()));
            for (const char *name : { "texture_size0", "texture_rot0", "texture_off0", "pixel_size0" }) {
                const GLint location = ::glGetUniformLocation(program, name);
                GLfloat values[16] {};
                if (location >= 0)
                    ::glGetUniformfv(program, location, values);
                std::fprintf(stderr, "native uniform: name=%s location=%d values=%g,%g,%g,%g\n", name, location,
                    double(values[0]), double(values[1]), double(values[2]), double(values[3]));
            }
            const GLint sampler = ::glGetUniformLocation(program, "texture0");
            if (sampler >= 0) {
                GLint unit = 0, active = 0, texture = 0, width = 0, height = 0;
                ::glGetUniformiv(program, sampler, &unit);
                ::glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
                ::glActiveTexture(GL_TEXTURE0 + unit);
                ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
                ::glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
                ::glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
                std::fprintf(stderr, "native sampler: unit=%d texture=%d size=%dx%d\n", unit, texture, width, height);
                ::glActiveTexture(active);
            }
        }
        ::glDrawArrays(mode, first, count);
    }
#endif

    void *getProcAddressGl(void *, const char *name)
    {
        QOpenGLContext *gl = QOpenGLContext::currentContext();
        if (!gl)
            return nullptr;
        void *address = reinterpret_cast<void *>(gl->getProcAddress(QByteArray(name)));
#ifdef Q_OS_TVOS
        if (!qgetenv("SPOOL_TEST_MPV_LOG").isEmpty()
            && (std::strcmp(name, "glViewport") == 0 || std::strcmp(name, "glVertexAttribPointer") == 0
                || std::strcmp(name, "glBindFramebuffer") == 0 || std::strcmp(name, "glUniformMatrix3fv") == 0)) {
            Dl_info provider {}, linked {};
            dladdr(address, &provider);
            dladdr(reinterpret_cast<void *>(&::glViewport), &linked);
            std::fprintf(stderr, "native GL provider: function=%s resolved=%s directGLES=%s\n", name,
                provider.dli_fname ? provider.dli_fname : "(unknown)", linked.dli_fname ? linked.dli_fname : "(unknown)");
        }
        if (!qgetenv("SPOOL_TEST_MPV_LOG").isEmpty()) {
            if (std::strcmp(name, "glDrawArrays") == 0)
                return reinterpret_cast<void *>(&diagnosticDrawArrays);
            if (std::strcmp(name, "glBufferData") == 0)
                return reinterpret_cast<void *>(&diagnosticBufferData);
        }
#endif
        return address;
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

        // Queue one GUI-owned acknowledgement only after GPU cleanup/handoff.
        // Publishing completion there keeps stack waiters out of renderer state.
        void completeHandoff()
        {
            m_pending.handle = nullptr;
            m_pending.dirty = false;
            if ((m_pending.releaseCompleted || m_pending.attachCompleted) && m_item) {
                QMetaObject::invokeMethod(
                    m_item,
                    [item = m_item, released = std::move(m_pending.releaseCompleted),
                        attached = std::move(m_pending.attachCompleted)] {
                        if (released)
                            released->store(true);
                        if (attached)
                            attached->store(true);
                        emit item->renderContextHandoffCompleted();
                    },
                    Qt::QueuedConnection);
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

#if !SPOOL_MPV_ITEM_RHI

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
#if defined(SPOOL_APPLE_MOBILE)
            if (!appleMobileRenderingAllowed())
                return;
#endif
            if (!m_lifecycle.item())
                return;

            // Qt owns the external-command bracket and binds this item's FBO.
            // Nested begin/end calls enqueue RHI state inside that bracket.
            if (m_lifecycle.hasPendingHandle()) {
                m_lifecycle.releaseContext();
                if (auto *next = m_lifecycle.nextHandle())
                    createRenderContext(next);
                QQuickOpenGLUtils::resetOpenGLState();
                if (auto *fbo = framebufferObject())
                    fbo->bind();
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

#ifdef Q_OS_TVOS
            static thread_local bool nativeReported = false;
            const int result = mpv_render_context_render(ctx, params);
            if (!nativeReported && !qgetenv("SPOOL_TEST_MPV_LOG").isEmpty()) {
                nativeReported = true;
                std::fprintf(stderr, "native renderer result: target=%dx%d status=%d error=%u\n",
                    mpfbo.w, mpfbo.h, result, unsigned(::glGetError()));
            }
#else
            mpv_render_context_render(ctx, params);
#endif
            QQuickOpenGLUtils::resetOpenGLState();
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
#ifdef SPOOL_WEBOS
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

#else // SPOOL_MPV_ITEM_RHI

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

            // Creating or destroying either an OpenGL or D3D11 renderer changes
            // shared native state, not just drawing a frame. Flush Qt's commands
            // before the handover and invalidate its cached bindings afterwards.
            const bool nativeStateHandoff = m_lifecycle.hasPendingHandle() && cb && rhi()
                && (rhi()->backend() == QRhi::OpenGLES2 || rhi()->backend() == QRhi::D3D11);
            if (nativeStateHandoff) {
                cb->beginExternal();
                if (rhi()->backend() == QRhi::OpenGLES2)
                    QQuickOpenGLUtils::resetOpenGLState();
            }
            if (m_lifecycle.hasPendingHandle()) {
                m_renderFailed = false;
                m_lifecycle.releaseContext();
                if (auto *next = m_lifecycle.nextHandle())
                    createRenderContext(next);
                if (nativeStateHandoff)
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
            // Qt's external boundary does not restore all GL raster state.
            // libmpv requires default blend/depth/scissor/color-mask state.
            if (rhi()->backend() == QRhi::OpenGLES2)
                QQuickOpenGLUtils::resetOpenGLState();
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

#if defined(SPOOL_MPV_ITEM_D3D11)
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
#if defined(SPOOL_MPV_ITEM_VULKAN)
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
#if defined(SPOOL_MPV_ITEM_D3D11)
            mpv_d3d11_init_params d3d11Init {};
            m_d3d11 = false;
#endif
#if defined(SPOOL_MPV_ITEM_VULKAN)
            mpv_vulkan_init_params vkInit {};
            m_vulkan = false;
#endif
            QRhi *rhi = this->rhi();
            if (!rhi) {
                qCritical() << "MpvVideoItem: no RHI to render through";
                return;
            }

#if defined(SPOOL_MPV_ITEM_D3D11)
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
#if defined(SPOOL_MPV_ITEM_VULKAN)
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
                if (rhi->backend() == QRhi::OpenGLES2)
                    QQuickOpenGLUtils::resetOpenGLState();
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

#if defined(SPOOL_MPV_ITEM_VULKAN)
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
#if defined(SPOOL_MPV_ITEM_D3D11)
            if (m_d3d11)
                return "Direct3D 11";
#endif
            return m_vulkan ? "Vulkan" : "OpenGL";
        }

        bool m_vulkan = false;
        bool m_d3d11 = false;
#if defined(SPOOL_MPV_ITEM_VULKAN)
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
#endif // SPOOL_MPV_ITEM_RHI
} // namespace

MpvVideoItem *MpvVideoItem::s_instance = nullptr;

MpvVideoItem::MpvVideoItem(QQuickItem *parent)
    : SPOOL_MPV_ITEM_BASE(parent)
{
#if SPOOL_MPV_ITEM_RHI
    // SDR stays in an 8-bit target. FP16 is reserved for a verified HDR
    // presentation surface, not selected from the video's metadata.
    setColorBufferFormat(TextureFormat::RGBA8);
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

void MpvVideoItem::setHdrOutput(bool enabled)
{
    if (m_hdrOutput == enabled)
        return;
    m_hdrOutput = enabled;
#if SPOOL_MPV_ITEM_RHI
    setColorBufferFormat(enabled ? TextureFormat::RGBA16F : TextureFormat::RGBA8);
#endif
    emit hdrOutputChanged();
    update();
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
    QObject::connect(this, &MpvVideoItem::renderContextHandoffCompleted, &waitLoop, [&] {
        if (completed->load())
            waitLoop.quit();
    });
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
    const auto completed = std::make_shared<std::atomic_bool>(false);
    QEventLoop releaseLoop;
    {
        QMutexLocker locker(&m_handleMutex);
        const bool needsRenderHandoff = m_renderCtxAtomic.load() || m_pendingHandle;
        m_pendingHandle = nullptr;
        m_handleDirty = true;
        if (!needsRenderHandoff)
            return true;
        m_releaseCompleted = completed;
    }

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &releaseLoop, &QEventLoop::quit);
    QObject::connect(this, &MpvVideoItem::renderContextHandoffCompleted, &releaseLoop, [&] {
        if (completed->load())
            releaseLoop.quit();
    });
    timeout.start(timeoutMs);
    update();
    releaseLoop.exec(QEventLoop::ExcludeUserInputEvents);
    return completed->load();
}

#if SPOOL_MPV_ITEM_RHI
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
    HandleSnapshot snap { m_pendingHandle, m_handleDirty, m_releaseCompleted, m_attachCompleted, m_pendingRenderBackend };
    m_handleDirty = false;
    m_releaseCompleted.reset();
    return snap;
}

} // namespace Spool
