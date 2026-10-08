#include "platform/PlatformPaths.h"

#include "TestMain.h"
#include "TestRequire.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#if defined(Q_OS_ANDROID) || defined(SPOOL_APPLE_MOBILE)
#include <QFontDatabase>
#include <QGuiApplication>
#include <QStandardPaths>
#endif
#include <QtGlobal>

#include <cstdlib>

namespace {

using SpoolTests::require;

} // namespace

SPOOL_TEST_MAIN("platform-paths")
{
#if defined(Q_OS_ANDROID) || defined(SPOOL_APPLE_MOBILE)
    QGuiApplication app(argc, argv);
    const QString appRoot = Spool::resolveAppRoot(argv[0]);
    const QString fontsPath = Spool::bundledFontsPath(appRoot);
#ifdef Q_OS_ANDROID
    const QString expectedFontsPath
        = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("fonts"));
    require(fontsPath == expectedFontsPath, "Android fonts must be extracted into the app's writable data directory");
    require(Spool::bundledFontsPath(QStringLiteral("/not-a-package-root")) == fontsPath,
        "Android font extraction must not depend on a desktop package root");
#else
    require(QFileInfo(appRoot).isDir(), "Apple's native bundle resource directory must exist");
    require(fontsPath == QDir(appRoot).filePath(QStringLiteral("fonts")),
        "Apple fonts must resolve inside the native bundle's resource directory");
#endif
    // QFile must read real files for the font consumers, not a resource URL.
    // Comparing against separately packaged source fixtures detects omitted,
    // truncated or stale extracted/bundled assets before trying the font loader.
    const QStringList fontFiles {
        QStringLiteral("AtkinsonHyperlegible-Bold.otf"),
        QStringLiteral("AtkinsonHyperlegible-Regular.otf"),
        QStringLiteral("IBMPlexSans-Variable.ttf"),
        QStringLiteral("PTRootUI-Variable.ttf"),
        QStringLiteral("MaterialIcons-Regular.ttf"),
    };
    require(QDir::isAbsolutePath(fontsPath) && !fontsPath.startsWith(QLatin1Char(':')),
        "mobile fonts must be available through a real absolute filesystem path");
    for (const QString& fontFile : fontFiles) {
        const QString path = QDir(fontsPath).filePath(fontFile);
        require(QFileInfo(path).isFile(), "a shipped mobile font is missing from the filesystem");
        QFile source(QDir(QStringLiteral(TEST_SOURCE_DIR "/qml/fonts")).filePath(fontFile));
        QFile font(path);
        require(source.open(QIODevice::ReadOnly) && font.open(QIODevice::ReadOnly),
            "a shipped mobile font or independent font fixture could not be opened");
        const QByteArray expectedBytes = source.readAll();
        require(!expectedBytes.isEmpty() && font.readAll() == expectedBytes,
            "mobile font extraction/packaging changed the shipped font bytes");
        const int fontId = QFontDatabase::addApplicationFont(path);
        require(fontId >= 0, "the application font loader could not load a shipped mobile font");
        require(!QFontDatabase::applicationFontFamilies(fontId).isEmpty(),
            "a shipped mobile font did not provide a usable font family");
        require(QFontDatabase::removeApplicationFont(fontId), "the test font registration could not be removed");
    }
#else
    QCoreApplication app(argc, argv);
    const QString appRoot = QDir::cleanPath(QDir::temp().filePath(QStringLiteral("spool-package-root")));
    require(Spool::bundledFontsPath(appRoot) == QDir(appRoot).filePath(QStringLiteral("fonts")),
        "desktop bundled fonts must resolve to the appRoot/fonts directory");
#endif
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
