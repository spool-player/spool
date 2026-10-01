#pragma once

#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>
#include <QQuickView>

#include <memory>

namespace Spool {

class InputLatencyMonitor;
class NativeAppWindow final : public QQuickView {
    Q_OBJECT
    Q_PROPERTY(int overlayRevision READ overlayRevision NOTIFY overlayRevisionChanged)
    Q_PROPERTY(int overlayX READ overlayX NOTIFY overlayRevisionChanged)
    Q_PROPERTY(int overlayY READ overlayY NOTIFY overlayRevisionChanged)
    Q_PROPERTY(int overlayWidth READ overlayWidth NOTIFY overlayRevisionChanged)
    Q_PROPERTY(int overlayHeight READ overlayHeight NOTIFY overlayRevisionChanged)
    Q_PROPERTY(qint64 systemMemoryBytes READ systemMemoryBytes CONSTANT)
    Q_PROPERTY(bool fullScreen READ fullScreen NOTIFY fullScreenChanged)
    Q_PROPERTY(bool hdrOutput READ hdrOutput NOTIFY hdrOutputChanged)
    Q_PROPERTY(qreal hdrSdrWhiteNits READ hdrSdrWhiteNits NOTIFY hdrOutputChanged)

public:
    explicit NativeAppWindow(const QString& appId, QWindow *parent = nullptr);
    ~NativeAppWindow() override;

    bool prepareForUiSurface();
    void setInputLatencyMonitor(InputLatencyMonitor *monitor);
    bool prepareForPlaybackSurface();
    // Direct playback puts video on a plane beneath this window, so the
    // window has to stop painting its own opaque background over it. A no-op
    // where video always goes through the scene graph.
    void setVideoUnderlayActive(bool active);
    // Bring the surface to the foreground. On webOS this re-issues
    // wl_webos_shell_surface_set_state(FULLSCREEN); on host Qt it
    // falls back to show()/requestActivate(). Safe to call from the
    // GUI thread at any point after prepareForUiSurface().
    void bringToFront();
    QString windowId() const;
    int overlayRevision() const;
    // The overlay is rasterised in the pixels the panel actually has, which is
    // what makes subtitles sharp, but QML is laid out in logical ones. On a
    // display that reports a ratio of one these are the same number and this
    // costs nothing; on a television reporting two, returning the raw size
    // would draw the subtitles at twice their width.
    int overlayX() const
    {
        return toLogical(m_overlayX);
    }
    int overlayY() const
    {
        return toLogical(m_overlayY);
    }
    int overlayWidth() const
    {
        return toLogical(m_overlayImage.width());
    }
    int overlayHeight() const
    {
        return toLogical(m_overlayImage.height());
    }
    bool fullScreen() const
    {
        return visibility() == QWindow::FullScreen || windowStates().testFlag(Qt::WindowFullScreen);
    }
    qint64 systemMemoryBytes() const
    {
        return m_systemMemoryBytes;
    }
    void setSystemMemoryBytes(qint64 bytes)
    {
        m_systemMemoryBytes = qMax<qint64>(0, bytes);
    }
    // Actual scRGB surface state, published on the GUI thread after the render
    // thread has inspected the swapchain. Independent of the playing file.
    bool hdrOutput() const
    {
        return m_hdrOutput;
    }
    qreal hdrSdrWhiteNits() const
    {
        return m_hdrSdrWhiteNits;
    }
    qreal hdrPeakNits() const
    {
        return m_hdrPeakNits;
    }
    void setHdrOutput(bool active, qreal sdrWhiteNits, qreal peakNits);
    Q_INVOKABLE void toggleFullScreen();
    // Whether the window should cover the system's own bars. Only Android
    // acts on it: elsewhere the window manager already decides, and the shell
    // asking has no meaning.
    Q_INVOKABLE void setImmersive(bool immersive);
    // Leave the app entirely and hand the screen back to whatever launched
    // it. Only a platform where that is a thing a user asks for implements
    // it; elsewhere a window is closed, not exited.
    Q_INVOKABLE void exitToLauncher();
#ifdef Q_OS_MACOS
    Q_INVOKABLE void setTitlebarVisible(bool visible);
#endif
    void clearOverlay();
    QQuickImageProvider *createOverlayImageProvider();
    QImage copyOverlayImage() const;

signals:
    void closeRequested();
    void overlayRevisionChanged();
    void fullScreenChanged();
    void hdrOutputChanged();
    void platformSurfaceStateChanged(int state);
    void platformSurfaceExposed(bool exposed);
    void platformCloseRequested();
    void pointerBackRequested();
    void pointerForwardRequested();

protected:
    bool event(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void exposeEvent(QExposeEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    bool ensureShellSurface();
    bool ensureVideoSurface();
    bool bindPlatformGlobals();
    void releasePlatformSurface();
    void requestPlatformFullscreen();
    void applyPlatformKeyMask();
    void updateCropRegion();
    void setVideoCrop(
        int origW, int origH, int srcX, int srcY, int srcW, int srcH, int dstX, int dstY, int dstW, int dstH);
    void scheduleVideoCrop(
        int origW, int origH, int srcX, int srcY, int srcW, int srcH, int dstX, int dstY, int dstW, int dstH);
    void publishPendingVideoCrop();
    void scheduleOverlayImage(QImage image, int x = 0, int y = 0);
    void publishPendingOverlayImage();
    void handlePlatformSurfaceCreated();
    void handlePlatformSurfaceAboutToBeDestroyed();

    struct PlatformData;
    friend struct PlatformData;

    InputLatencyMonitor *m_inputLatencyMonitor = nullptr;
    QString m_appId;
    mutable QMutex m_overlayMutex;
    int toLogical(int devicePixels) const
    {
        const qreal ratio = devicePixelRatio();
        return ratio > 0 ? qRound(devicePixels / ratio) : devicePixels;
    }

    QImage m_overlayImage;
    QImage m_pendingOverlayImage;
    int m_overlayX = 0;
    int m_overlayY = 0;
    int m_pendingOverlayX = 0;
    int m_pendingOverlayY = 0;
    bool m_overlayPublishQueued = false;
    int m_overlayRevision = 0;
    qint64 m_systemMemoryBytes = 0;
    bool m_hdrOutput = false;
    qreal m_hdrSdrWhiteNits = 203.0;
    qreal m_hdrPeakNits = 0.0;
    Qt::MouseButton m_pointerNavigationButton = Qt::NoButton;
    std::unique_ptr<PlatformData> m_platform;
    bool m_immersive = false;
};

} // namespace Spool
