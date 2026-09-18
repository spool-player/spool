#include "diagnostics/InputLatencyMonitor.h"
#include "platform/NativeAppWindow.h"

#include <QCloseEvent>
#include <QDebug>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QMutexLocker>
#include <QPlatformSurfaceEvent>
#include <QQuickImageProvider>

namespace JellyfinNative {

namespace {

    class OverlayImageProvider final : public QQuickImageProvider {
    public:
        explicit OverlayImageProvider(const NativeAppWindow *window)
            : QQuickImageProvider(QQuickImageProvider::Image)
            , m_window(window)
        {
        }

        QImage requestImage(const QString&, QSize *size, const QSize& requestedSize) override
        {
            QImage image = m_window->copyOverlayImage();
            if (image.isNull()) {
                image = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::transparent);
            }
            if (requestedSize.isValid() && requestedSize != image.size())
                image = image.scaled(requestedSize, Qt::IgnoreAspectRatio, Qt::FastTransformation);
            if (size)
                *size = image.size();
            return image;
        }

    private:
        const NativeAppWindow *m_window = nullptr;
    };

} // namespace

void NativeAppWindow::setInputLatencyMonitor(InputLatencyMonitor *monitor)
{
    m_inputLatencyMonitor = monitor;
}

void NativeAppWindow::setHdrOutput(bool active, qreal sdrWhiteNits, qreal peakNits)
{
    if (m_hdrOutput == active && qFuzzyCompare(m_hdrSdrWhiteNits, sdrWhiteNits)
        && qFuzzyCompare(m_hdrPeakNits + 1.0, peakNits + 1.0))
        return;
    m_hdrOutput = active;
    m_hdrSdrWhiteNits = sdrWhiteNits;
    m_hdrPeakNits = peakNits;
    emit hdrOutputChanged();
    update();
}

bool NativeAppWindow::event(QEvent *event)
{
    if (event->type() == QEvent::PlatformSurface) {
        const auto *surfaceEvent = static_cast<const QPlatformSurfaceEvent *>(event);
        if (surfaceEvent->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
            handlePlatformSurfaceAboutToBeDestroyed();
    }

    const quint64 token = m_inputLatencyMonitor ? m_inputLatencyMonitor->beginInput(event) : 0;
    const auto finishInput = [this, token] {
        if (m_inputLatencyMonitor)
            m_inputLatencyMonitor->endInput(token);
    };

    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        const Qt::MouseButton button = mouseEvent->button();
        const bool navigationButton = button == Qt::BackButton || button == Qt::ForwardButton;
        if (navigationButton) {
            if (event->type() == QEvent::MouseButtonPress) {
                m_pointerNavigationButton = button;
                if (mouseEvent->source() == Qt::MouseEventNotSynthesized) {
                    if (button == Qt::BackButton)
                        emit pointerBackRequested();
                    else
                        emit pointerForwardRequested();
                }
            } else if (m_pointerNavigationButton == button) {
                m_pointerNavigationButton = Qt::NoButton;
            }
            mouseEvent->accept();
            finishInput();
            return true;
        }
    }

    const bool handled = QQuickView::event(event);
    if (event->type() == QEvent::PlatformSurface) {
        const auto *surfaceEvent = static_cast<const QPlatformSurfaceEvent *>(event);
        if (surfaceEvent->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated)
            handlePlatformSurfaceCreated();
    }
    finishInput();
    return handled;
}

void NativeAppWindow::closeEvent(QCloseEvent *event)
{
    emit closeRequested();
    QQuickView::closeEvent(event);
}

int NativeAppWindow::overlayRevision() const
{
    return m_overlayRevision;
}

void NativeAppWindow::clearOverlay()
{
    bool changed = false;
    {
        QMutexLocker locker(&m_overlayMutex);
        m_pendingOverlayImage = QImage();
        m_pendingOverlayX = 0;
        m_pendingOverlayY = 0;
        if (m_overlayImage.isNull())
            return;
        m_overlayImage = QImage();
        m_overlayX = 0;
        m_overlayY = 0;
        ++m_overlayRevision;
        changed = true;
    }
    if (changed)
        emit overlayRevisionChanged();
}

void NativeAppWindow::scheduleOverlayImage(QImage image, int x, int y)
{
    bool shouldQueue = false;
    {
        QMutexLocker locker(&m_overlayMutex);
        m_pendingOverlayImage = std::move(image);
        m_pendingOverlayX = x;
        m_pendingOverlayY = y;
        if (!m_overlayPublishQueued) {
            m_overlayPublishQueued = true;
            shouldQueue = true;
        }
    }
    if (shouldQueue)
        QMetaObject::invokeMethod(this, [this]() { publishPendingOverlayImage(); }, Qt::QueuedConnection);
}

void NativeAppWindow::publishPendingOverlayImage()
{
    bool changed = false;
    {
        QMutexLocker locker(&m_overlayMutex);
        QImage image = std::move(m_pendingOverlayImage);
        const int x = m_pendingOverlayX;
        const int y = m_pendingOverlayY;
        m_pendingOverlayImage = QImage();
        m_overlayPublishQueued = false;
        if (image.isNull() && m_overlayImage.isNull())
            return;
        // An overlay that is drawn but never composited, and one that is never
        // drawn at all, look exactly alike from outside the app: a picture with
        // no subtitles on it. Say when it starts and when it stops.
        if (m_overlayImage.isNull() != image.isNull()) {
            if (image.isNull())
                qInfo() << "overlay: mpv overlay cleared";
            else
                qInfo().nospace() << "overlay: mpv drew " << image.width() << "x" << image.height() << " at " << x
                                  << "," << y;
        }
        m_overlayImage = std::move(image);
        m_overlayX = x;
        m_overlayY = y;
        ++m_overlayRevision;
        changed = true;
    }
    if (changed)
        emit overlayRevisionChanged();
}

QQuickImageProvider *NativeAppWindow::createOverlayImageProvider()
{
    return new OverlayImageProvider(this);
}

QImage NativeAppWindow::copyOverlayImage() const
{
    QMutexLocker locker(&m_overlayMutex);
    return m_overlayImage;
}

} // namespace JellyfinNative
