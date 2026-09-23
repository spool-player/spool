#include "platform/PlatformPaths.h"

#include "TestMain.h"

#include <QDir>
#include <QFile>
#include <QtGlobal>

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

} // namespace

SPOOL_TEST_MAIN("platform-paths")
{
    const QString appRoot = QDir::cleanPath(QDir::temp().filePath(QStringLiteral("spool-package-root")));
    require(Spool::bundledFontsPath(appRoot) == QDir(appRoot).filePath(QStringLiteral("fonts")),
        "bundled fonts must resolve to the platform-neutral appRoot/fonts directory");
#ifdef Q_OS_WIN
    const QString localAppData = QDir::cleanPath(QDir::temp().filePath(QStringLiteral("spool-local-app-data")));
    qputenv("LOCALAPPDATA", QFile::encodeName(localAppData));
    qunsetenv("SPOOL_CACHE_HOME");
    const QString spoolRoot = QDir(localAppData).filePath(QStringLiteral("spool-jellyfin"));
    require(Spool::persistentDataRoot() == QDir(spoolRoot).filePath(QStringLiteral("data")),
        "Windows persistent data must use LOCALAPPDATA/spool-jellyfin/data");
    require(Spool::startupCacheRoot(appRoot) == QDir(spoolRoot).filePath(QStringLiteral("cache")),
        "Windows caches must use LOCALAPPDATA/spool-jellyfin/cache");
    require(Spool::appLogDirectories(appRoot) == QStringList { QDir(spoolRoot).filePath(QStringLiteral("logs")) },
        "Windows logs must use LOCALAPPDATA/spool-jellyfin/logs");
#endif
    return EXIT_SUCCESS;
}
