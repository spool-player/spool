#include "GraphicsStartup.h"

#include "platform/NativeAppWindow.h"
#include "platform/PlatformDisplayOutput.h"
#include "platform/PlatformStartup.h"
#include "player/RenderTargetProfile.h"
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID) && !defined(JELLYFIN_NATIVE_WEBOS)
#include "platform/linux/WaylandColorInfo.h"
#endif

#include <QDebug>
#include <QGuiApplication>
#include <QMetaObject>
#include <QQuickWindow>
#include <QSurfaceFormat>

namespace JellyfinNative {

QSGRendererInterface::GraphicsApi GraphicsStartup::configureBeforeApplication(bool launchTest)
{
    // Keep Qt and mpv on the same API. OpenGL remains the explicit SDR
    // compatibility path; Linux and macOS use Vulkan for HDR-capable rendering.
#if defined(Q_OS_WIN)
    auto graphicsApi = QSGRendererInterface::Direct3D11;
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID) && !defined(JELLYFIN_NATIVE_WEBOS) && QT_CONFIG(vulkan)
    auto graphicsApi = QSGRendererInterface::Vulkan;
#elif defined(Q_OS_MACOS) && QT_CONFIG(vulkan)
    auto graphicsApi = QSGRendererInterface::Vulkan;
#else
    auto graphicsApi = QSGRendererInterface::OpenGL;
#endif

    // The persisted Auto choice keeps the platform default. A developer's
    // environment override wins, including when inherited by a self-restart.
    QByteArray requestedApi = qgetenv("SPOOL_RENDER_API").toLower();
    if (!requestedApi.isEmpty()) {
        qInfo("startup: SPOOL_RENDER_API=%s overrides the graphics backend setting", requestedApi.constData());
    } else {
        const auto stored = RenderTargetPolicy::startupGraphicsApi();
        if (stored != RenderTargetPolicy::GraphicsApiPreference::Automatic) {
            requestedApi = RenderTargetPolicy::graphicsApiName(stored);
            qInfo("startup: graphics backend setting asks for %s", requestedApi.constData());
        }
    }
    if (launchTest) {
        // Headless launch tests cannot assume any graphics adapter exists.
        graphicsApi = QSGRendererInterface::Software;
    } else if (requestedApi == "opengl") {
        graphicsApi = QSGRendererInterface::OpenGL;
    } else if (requestedApi == "d3d11" || requestedApi == "direct3d11") {
#if defined(Q_OS_WIN)
        graphicsApi = QSGRendererInterface::Direct3D11;
#else
        qInfo("startup: SPOOL_RENDER_API asked for Direct3D 11, which only Windows has");
#endif
    } else if (requestedApi == "vulkan") {
#if QT_CONFIG(vulkan)
        graphicsApi = QSGRendererInterface::Vulkan;
#else
        qInfo("startup: SPOOL_RENDER_API asked for Vulkan, which this build has no support for");
#endif
    }
#if defined(Q_OS_MACOS)
    if (graphicsApi == QSGRendererInterface::Vulkan) {
        // QTBUG-149443: MoltenVK must present through a plain CAMetalLayer.
        qputenv("QT_MTL_NO_TRANSACTION", QByteArrayLiteral("1"));
    }
#endif
    QQuickWindow::setGraphicsApi(graphicsApi);
    qInfo("startup: scene graph on %s",
        graphicsApi == QSGRendererInterface::Vulkan           ? "Vulkan"
            : graphicsApi == QSGRendererInterface::Direct3D11 ? "Direct3D 11"
            : graphicsApi == QSGRendererInterface::Software   ? "software"
                                                              : "OpenGL");
    QSurfaceFormat::setDefaultFormat(platformSurfaceFormat());
    return graphicsApi;
}

QByteArray GraphicsStartup::prepareBeforeWindow(QSGRendererInterface::GraphicsApi graphicsApi, bool launchTest)
{
    bool automaticHdr = false;
    bool allowHdrRequest = !launchTest
        && (graphicsApi == QSGRendererInterface::Vulkan || graphicsApi == QSGRendererInterface::Direct3D11);
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID) && !defined(JELLYFIN_NATIVE_WEBOS)
    allowHdrRequest = allowHdrRequest && QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
    automaticHdr = allowHdrRequest;
#elif defined(Q_OS_WIN)
    // Qt checks the window's output and Windows' Use HDR state before choosing
    // FP16. The live probe verifies the actual buffer and native signaling.
    // Vulkan requests the same scRGB encoding through swapchain colorspace.
    automaticHdr = allowHdrRequest;
#endif
    const QByteArray hdrRequest
        = allowHdrRequest ? RenderTargetPolicy::startupSwapChainRequest(automaticHdr) : QByteArray();
#if !defined(Q_OS_ANDROID) && !defined(JELLYFIN_NATIVE_WEBOS)
    if (!hdrRequest.isEmpty()) {
        qputenv("QSG_RHI_HDR", hdrRequest);
        qInfo("startup: set QSG_RHI_HDR=%s in environment", hdrRequest.constData());
    } else {
        qunsetenv("QSG_RHI_HDR");
    }
#endif
    return hdrRequest;
}

void GraphicsStartup::attachToWindow(NativeAppWindow& window, const QByteArray& hdrRequest)
{
    new GraphicsStartup(window, hdrRequest);
}

GraphicsStartup::GraphicsStartup(NativeAppWindow& window, const QByteArray& hdrRequest)
    : QObject(&window)
    , m_window(window)
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID) && !defined(JELLYFIN_NATIVE_WEBOS)
    m_waylandHdrSurface = std::make_unique<WaylandHdrSurface>(&window);
#endif
    window.setProperty("_qt_sg_hdr_format", hdrRequest);
    qInfo("startup: output request=%s qpa=%s", hdrRequest.isEmpty() ? "SDR" : hdrRequest.constData(),
        qPrintable(QGuiApplication::platformName()));
#if !defined(Q_OS_ANDROID) && !defined(JELLYFIN_NATIVE_WEBOS)
    // QQuickWindow stops its scene graph before destroying its QObject children.
    // Direct callbacks therefore retain a live owner through render shutdown;
    // queued GUI updates are cancelled automatically when the owner dies.
    connect(
        &window, &QQuickWindow::sceneGraphInitialized, this, [this] { m_outputSnapshotPending = true; },
        Qt::DirectConnection);
    connect(&window, &QQuickWindow::beforeSynchronizing, this, &GraphicsStartup::probeSwapchain, Qt::DirectConnection);
#endif
}

GraphicsStartup::~GraphicsStartup() = default;

void GraphicsStartup::probeSwapchain()
{
#if !defined(Q_OS_ANDROID) && !defined(JELLYFIN_NATIVE_WEBOS)
    // QRhi access stays on the render thread. Retry until the swapchain exists,
    // then snapshot once per scene graph, not on every frame. Output-change
    // subscriptions are deliberately separate from this startup observation.
    if (!m_outputSnapshotPending)
        return;
    auto display = PlatformDisplayOutput::probe(&m_window);
    if (!display.surfaceReady)
        return;
    m_outputSnapshotPending = false;
    QMetaObject::invokeMethod(
        this,
        [this, display]() mutable {
            // Native display queries, Wayland protocol ownership and QML
            // notifications belong to the GUI thread, never the render thread.
            PlatformDisplayOutput::updateDisplayLuminance(display, &m_window);
            bool scrgb
                = display.hdrAvailable && display.preferredFormat == RenderTargetProfile::Format::ExtendedSrgbLinear;
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID) && !defined(JELLYFIN_NATIVE_WEBOS)
            if (!m_waylandHdrSurface->setEnabled(scrgb && display.needsWaylandDescription))
                scrgb = false;
#endif
            const qreal peak = display.luminanceMeasured ? display.maxLuminanceNits : 0.0;
            m_window.setHdrOutput(scrgb, display.sdrWhiteNits, peak);
            qInfo("output: swapchain=%s supported=%d sdrWhite=%.1f peak=%.1f luminanceReported=%s",
                scrgb ? "scRGB" : "SDR", static_cast<int>(display.supportedFormat), double(display.sdrWhiteNits),
                double(peak), display.luminanceMeasured ? "yes" : "no");
        },
        Qt::QueuedConnection);
#endif
}

} // namespace JellyfinNative
