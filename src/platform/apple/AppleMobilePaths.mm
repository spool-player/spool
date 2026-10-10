#include "platform/PlatformPaths.h"

#include <QDir>
#include <QStandardPaths>
#import <Foundation/Foundation.h>
#import <TargetConditionals.h>

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
#if TARGET_OS_TV
    // tvOS only permits purgeable cache/tmp storage. Tokens remain separately
    // in Keychain; the database and downloaded artwork must tolerate eviction.
    const QString root = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
                             .filePath(QStringLiteral("data"));
#else
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
#endif
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
