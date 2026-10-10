#include "platform/PlatformPaths.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace Spool {

QString resolveAppRoot(const char *argv0)
{
    QString executable = QFileInfo(QStringLiteral("/proc/self/exe")).symLinkTarget();
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

QString startupCacheRoot(const QString& appRootPath)
{
    const QByteArray configured = qgetenv("SPOOL_CACHE_HOME");
    if (!configured.isEmpty())
        return QString::fromLocal8Bit(configured);
    const QByteArray xdgCache = qgetenv("XDG_CACHE_HOME");
    if (!xdgCache.isEmpty())
        return QDir(QString::fromLocal8Bit(xdgCache)).filePath(QStringLiteral("com.sachk.spool"));
    return QDir(appRootPath).filePath(QStringLiteral(".cache"));
}

QString persistentDataRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QStringList appLogDirectories(const QString& appRootPath)
{
    return { QStringLiteral("/tmp"), QDir(appRootPath).filePath(QStringLiteral(".cache/logs")) };
}

QString appLogFileName()
{
    return QStringLiteral("com.sachk.spool.log");
}

} // namespace Spool
