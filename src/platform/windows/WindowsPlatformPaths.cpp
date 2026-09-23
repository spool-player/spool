#include "platform/PlatformPaths.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <windows.h>

namespace Spool {

namespace {
    QString spoolRoot()
    {
        const QString configured = QString::fromLocal8Bit(qgetenv("LOCALAPPDATA"));
        return configured.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                                    : QDir(configured).filePath(QStringLiteral("spool-jellyfin"));
    }
} // namespace

QString resolveAppRoot(const char *)
{
    wchar_t executablePath[32768] {};
    const DWORD length = GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
    if (length == 0 || length >= std::size(executablePath))
        return {};
    return QFileInfo(QString::fromWCharArray(executablePath, static_cast<qsizetype>(length))).absolutePath();
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
    return QDir(spoolRoot()).filePath(QStringLiteral("cache"));
}

QString persistentDataRoot()
{
    return QDir(spoolRoot()).filePath(QStringLiteral("data"));
}

QStringList appLogDirectories(const QString&)
{
    return { QDir(spoolRoot()).filePath(QStringLiteral("logs")) };
}

QString appLogFileName()
{
    return QStringLiteral("spool.log");
}

} // namespace Spool
