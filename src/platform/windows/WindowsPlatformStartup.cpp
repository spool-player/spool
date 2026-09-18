#include "platform/PlatformStartup.h"

#include "platform/NativeAppWindow.h"

#include <QByteArray>

namespace JellyfinNative {

bool configurePlatformEnvironment(const QString&)
{
    if (qEnvironmentVariableIsSet("JELLYFIN_NATIVE_VERBOSE_QT")) {
        qputenv("QT_DEBUG_PLUGINS", QByteArrayLiteral("1"));
        qputenv("QT_LOGGING_RULES",
            QByteArrayLiteral("qt.qml*=true;qt.qpa*=true;qt.scenegraph*=true;qt.quick*=true;qt.plugin*=true"));
    }
    return true;
}

QSurfaceFormat platformSurfaceFormat()
{
    QSurfaceFormat format;
    // Only the OpenGL compatibility path reads these two.
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    // No alpha channel: Qt turns a non-zero alpha request into a swapchain
    // flagged as having premultiplied alpha, and the compositor then blends
    // the window against the desktop. On the scRGB FP16 swapchain that shows
    // the desktop through the player's background and its letterbox bars. The
    // window is opaque, so say so.
    format.setAlphaBufferSize(0);
    return format;
}

void configurePlatformWindow(NativeAppWindow&) { }

} // namespace JellyfinNative
