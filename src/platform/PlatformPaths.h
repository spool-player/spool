#pragma once

#include <QString>
#include <QStringList>

namespace Spool {

QString resolveAppRoot(const char *argv0);
QString bundledFontsPath(const QString& appRootPath);
QString startupCacheRoot(const QString& appRootPath);
QString persistentDataRoot();
QStringList appLogDirectories(const QString& appRootPath);
QString appLogFileName();

} // namespace Spool
