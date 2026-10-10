#include "platform/PlatformStartup.h"

#include "platform/NativeAppWindow.h"

#include <cstdlib>
#include <unistd.h>

namespace Spool {

namespace {
    bool ensureWaylandEnvironment()
    {
        const char *runtimeDir = std::getenv("XDG_RUNTIME_DIR");
        const char *display = std::getenv("WAYLAND_DISPLAY");
        if ((!runtimeDir || !runtimeDir[0]) && access("/tmp/xdg", X_OK) == 0) {
            setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 1);
            runtimeDir = "/tmp/xdg";
        }
        if ((!display || !display[0]) && runtimeDir && runtimeDir[0]) {
            const QByteArray socket = QByteArray(runtimeDir) + "/wayland-0";
            if (access(socket.constData(), F_OK) == 0) {
                setenv("WAYLAND_DISPLAY", "wayland-0", 1);
                display = "wayland-0";
            }
        }
        return runtimeDir && runtimeDir[0] && display && display[0];
    }
} // namespace

bool configurePlatformEnvironment(const QString&)
{
    setenv("APPID", "com.sachk.spool", 1);
    setenv("MALLOC_ARENA_MAX", "2", 0);
    setenv("DISPLAY_ID", "0", 1);
    setenv("QT_QPA_PLATFORM", "wayland-egl", 1);
    setenv("QSG_RHI_BACKEND", "opengl", 1);
    unsetenv("QT_QUICK_BACKEND");
    unsetenv("QMLSCENE_DEVICE");
    // webOS exports this for its Qt 5 applications, but Qt 6 also uses it to
    // reject embedded qmlcachegen units. Our QML source is discarded, so the
    // launcher-provided value would leave every QML resource empty.
    unsetenv("QML_DISABLE_DISK_CACHE");
    unsetenv("QT_IM_MODULES");
    setenv("QT_IM_MODULE", "webosim", 1);
    setenv("QT_WAYLAND_SHELL_INTEGRATION", "wl-shell", 1);
    setenv("QT_QPA_FONTDIR", "/usr/share/fonts", 1);
    setenv("QT_NO_GLIB", "1", 1);
    setenv("SPOOL_QT_NO_CURSOR_SURFACE", "1", 1);
    if (qEnvironmentVariableIsSet("SPOOL_VERBOSE_QT")) {
        setenv("QT_DEBUG_PLUGINS", "1", 1);
        setenv("QT_LOGGING_RULES", "qt.qml*=true;qt.qpa*=true;qt.scenegraph*=true;qt.quick*=true;qt.plugin*=true", 1);
    } else {
        unsetenv("QT_DEBUG_PLUGINS");
        unsetenv("QT_LOGGING_RULES");
    }
    // Keep video FBO rendering off the GUI thread. The basic loop makes every
    // QML or diagnostics stall a missed mpv presentation deadline.
    if (!qEnvironmentVariableIsSet("QSG_RENDER_LOOP"))
        qputenv("QSG_RENDER_LOOP", QByteArrayLiteral("threaded"));
    return ensureWaylandEnvironment();
}

QSurfaceFormat platformSurfaceFormat()
{
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(2, 0);
    format.setAlphaBufferSize(8);
    return format;
}

void configurePlatformWindow(NativeAppWindow& window)
{
    window.setPersistentGraphics(false);
    window.setPersistentSceneGraph(false);
}

} // namespace Spool
