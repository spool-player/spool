#include "platform/PlatformStartup.h"

#include "platform/NativeAppWindow.h"

#include <QByteArray>

namespace Spool {

bool configurePlatformEnvironment(const QString&)
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("wayland"));
    if (qEnvironmentVariableIsSet("SPOOL_VERBOSE_QT")) {
        qputenv("QT_DEBUG_PLUGINS", QByteArrayLiteral("1"));
        qputenv("QT_LOGGING_RULES",
            QByteArrayLiteral("qt.qml*=true;qt.qpa*=true;qt.scenegraph*=true;qt.quick*=true;qt.plugin*=true"));
    }
    return true;
}

QSurfaceFormat platformSurfaceFormat()
{
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    // An opaque surface is what lets Qt Quick hand native text to the system's
    // subpixel antialiasing; an alpha channel drops it back to grayscale, which
    // is what made small labels look ragged next to the rest of the desktop.
    // Nothing shows through the window — it is black behind the video.
    format.setAlphaBufferSize(0);
    return format;
}

void configurePlatformWindow(NativeAppWindow&) { }

} // namespace Spool
