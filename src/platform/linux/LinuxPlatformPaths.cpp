#include "platform/PlatformPaths.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace Spool {

QString resolveAppRoot(const char *argv0)
{
    QString executable = QFileInfo(QStringLiteral("/proc/self/exe")).symLinkTarget();
    // The AppImage may start the app through its bundled glibc's loader, which
    // /proc/self/exe then names; argv[0] is still the app.
    if (QFileInfo(executable).fileName().startsWith(QLatin1String("ld-linux")))
        executable.clear();
    if (executable.isEmpty() && argv0)
        executable = QFileInfo(QString::fromLocal8Bit(argv0)).canonicalFilePath();
    if (executable.isEmpty())
        return {};
    return QDir::cleanPath(QDir(QFileInfo(executable).absolutePath()).absoluteFilePath(QStringLiteral("..")));
}

QString bundledFontsPath(const QString& appRootPath)
{
    return QDir(appRootPath).filePath(QStringLiteral("fonts"));
}

QString startupCacheRoot(const QString&)
{
    const QByteArray configured = qgetenv("SPOOL_CACHE_HOME");
    if (!configured.isEmpty())
        return QString::fromLocal8Bit(configured);
    const QByteArray xdgCache = qgetenv("XDG_CACHE_HOME");
    if (!xdgCache.isEmpty())
        return QDir(QString::fromLocal8Bit(xdgCache)).filePath(QStringLiteral("com.sachk.spool"));
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

QString persistentDataRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QStringList appLogDirectories(const QString&)
{
    return {
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("logs"))
    };
}

QString appLogFileName()
{
    return QStringLiteral("spool.log");
}

} // namespace Spool
