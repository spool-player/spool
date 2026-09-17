#include "platform/PlatformStartup.h"

#include "platform/NativeAppWindow.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace JellyfinNative {

bool configurePlatformEnvironment(const QString& appRootPath)
{
    // resolveAppRoot() returns the executable directory's parent: Contents for
    // an app bundle, but a build/install root for an unbundled executable.
    // Only bundles carry a private loader and driver; development launches must
    // retain the environment's normal Vulkan discovery.
    const QDir contents(appRootPath);
    const QFileInfo bundle(QDir::cleanPath(contents.absoluteFilePath(QStringLiteral(".."))));
    if (contents.dirName() == QStringLiteral("Contents")
        && bundle.fileName().endsWith(QStringLiteral(".app"), Qt::CaseInsensitive)
        && QFileInfo(contents.filePath(QStringLiteral("Info.plist"))).isFile()
        && QFileInfo(contents.filePath(QStringLiteral("MacOS"))).isDir()) {
        const QString loader = contents.filePath(QStringLiteral("Frameworks/libvulkan.1.dylib"));
        if (!qEnvironmentVariableIsSet("QT_VULKAN_LIB") && QFileInfo(loader).isFile())
            qputenv("QT_VULKAN_LIB", QFile::encodeName(loader));

        const QString icd = contents.filePath(QStringLiteral("Resources/vulkan/icd.d/MoltenVK_icd.json"));
        // VK_DRIVER_FILES supersedes the legacy override and additive search
        // paths, so leave all three user choices untouched.
        if (!qEnvironmentVariableIsSet("VK_DRIVER_FILES") && !qEnvironmentVariableIsSet("VK_ICD_FILENAMES")
            && !qEnvironmentVariableIsSet("VK_ADD_DRIVER_FILES") && QFileInfo(icd).isFile())
            qputenv("VK_DRIVER_FILES", QFile::encodeName(icd));
    }

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
    format.setRenderableType(QSurfaceFormat::OpenGL);
    // macOS exposes modern OpenGL only through a core profile. A profile-less
    // request can leave Qt with legacy GL, which libplacebo cannot initialize.
    format.setVersion(4, 1);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setAlphaBufferSize(8);
    return format;
}

void configurePlatformWindow(NativeAppWindow&) { }

} // namespace JellyfinNative
