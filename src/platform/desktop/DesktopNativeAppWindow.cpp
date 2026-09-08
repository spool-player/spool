#include "platform/NativeAppWindow.h"

#include <QExposeEvent>
#include <QRegularExpression>
#include <QResizeEvent>

namespace JellyfinNative {
struct NativeAppWindow::PlatformData { };

NativeAppWindow::NativeAppWindow(const QString& appId, QWindow *parent)
    : QQuickView(parent)
    , m_appId(appId)
    , m_platform(std::make_unique<PlatformData>())
{
    setColor(Qt::black);
    setResizeMode(QQuickView::SizeRootObjectToView);
    setTitle(QStringLiteral("Spool for Jellyfin"));
#ifdef Q_OS_MACOS
    setFlags(flags() | Qt::ExpandedClientAreaHint | Qt::NoTitleBarBackgroundHint);
#endif

    // SPOOL_WINDOW_SIZE=1200x1200 opens on a shape no window manager will
    // hand you by dragging. The layout is meant to flow into any of them, and
    // this is how that gets looked at and measured rather than assumed.
    QSize initial(1280, 720);
    static const QRegularExpression geometry(QStringLiteral("^(\\d{2,5})[xX](\\d{2,5})$"));
    const QRegularExpressionMatch match = geometry.match(qEnvironmentVariable("SPOOL_WINDOW_SIZE").trimmed());
    if (match.hasMatch())
        initial = QSize(match.captured(1).toInt(), match.captured(2).toInt());
    resize(initial);
}

NativeAppWindow::~NativeAppWindow() = default;

void NativeAppWindow::setVideoUnderlayActive(bool)
{
    // Nothing to get out of the way of here.
}

bool NativeAppWindow::prepareForUiSurface()
{
    if (!isVisible())
        show();
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
        show();
    raise();
    requestActivate();
}

void NativeAppWindow::toggleFullScreen()
{
    if (fullScreen())
        showNormal();
    else
        showFullScreen();
    requestActivate();
    emit fullScreenChanged();
}

// The desktop window manager owns this decision; the shell only ever asks
// because Android needs it to.
void NativeAppWindow::setImmersive(bool immersive)
{
    m_immersive = immersive;
}

void NativeAppWindow::exitToLauncher()
{
    close();
}

QString NativeAppWindow::windowId() const
{
    return { };
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

} // namespace JellyfinNative
