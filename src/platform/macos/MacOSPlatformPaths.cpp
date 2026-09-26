#include "platform/PlatformPaths.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <mach-o/dyld.h>

namespace Spool {

QString resolveAppRoot(const char *argv0)
{
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    QByteArray buffer(static_cast<qsizetype>(size), '\0');
    QString executable;
    if (size > 0 && _NSGetExecutablePath(buffer.data(), &size) == 0)
        executable = QFileInfo(QString::fromLocal8Bit(buffer.constData())).canonicalFilePath();
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
