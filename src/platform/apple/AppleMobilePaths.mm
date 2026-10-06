#include "platform/PlatformPaths.h"

#import <Foundation/Foundation.h>
#include <QDir>
#include <QStandardPaths>

namespace Spool {
QString resolveAppRoot(const char *)
{
    return QString::fromUtf8(NSBundle.mainBundle.resourcePath.UTF8String);
}
QString bundledFontsPath(const QString& root)
{
    return QDir(root).filePath(QStringLiteral("fonts"));
}
QString startupCacheRoot(const QString&)
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}
QString persistentDataRoot()
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(root);
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:root.toUtf8().constData()]];
    [url setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
    return root;
}
QStringList appLogDirectories(const QString&)
{
    return { QDir(startupCacheRoot({})).filePath(QStringLiteral("logs")) };
}
QString appLogFileName()
{
    return QStringLiteral("spool.log");
}
} // namespace Spool
