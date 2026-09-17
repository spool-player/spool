#pragma once

#include <QByteArray>
#include <QObject>
#include <QSGRendererInterface>

#include <memory>

namespace JellyfinNative {

class NativeAppWindow;
class WaylandHdrSurface;

// The startup request is distinct from the live swapchain result: asking for
// HDR never makes a window HDR until the render-thread probe confirms it.
class GraphicsStartup final : public QObject {
public:
    // Call after setting the QCoreApplication identity, before QGuiApplication.
    static QSGRendererInterface::GraphicsApi configureBeforeApplication(bool launchTest);

    // Call after QGuiApplication selects the QPA plugin, before constructing the
    // window. Qt can consume QSG_RHI_HDR while setting up the native surface.
    static QByteArray prepareBeforeWindow(QSGRendererInterface::GraphicsApi graphicsApi, bool launchTest);

    // Call on the GUI thread before exposing the window. The window owns the
    // monitor so its Wayland description survives until native surface teardown.
    static void attachToWindow(NativeAppWindow& window, const QByteArray& hdrRequest);
    ~GraphicsStartup() override;

private:
    GraphicsStartup(NativeAppWindow& window, const QByteArray& hdrRequest);
    void probeSwapchain();

    NativeAppWindow& m_window;
    // Accessed only by direct scene-graph callbacks on the render thread.
    bool m_outputSnapshotPending = true;
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID) && !defined(JELLYFIN_NATIVE_WEBOS)
    std::unique_ptr<WaylandHdrSurface> m_waylandHdrSurface;
#endif
};

} // namespace JellyfinNative
