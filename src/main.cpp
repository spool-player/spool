#include "api/JellyfinProvider.h"
#include "app/AppController.h"
#include "app/ArtworkImageProvider.h"
#include "app/ArtworkService.h"
#include "app/CpuTopology.h"
#include "app/LocalizationManager.h"
#include "app/MemoryBudget.h"
#include "app/RouterController.h"
#include "app/UserItemStateController.h"
#include "cache/DatabaseManager.h"
#include "common/LogRotation.h"
#include "common/TlsTrust.h"
#include "diagnostics/Diagnostics.h"
#include "diagnostics/InputLatencyMonitor.h"
#include "diagnostics/RenderBenchmark.h"
#include "diagnostics/SystemPerformanceMonitor.h"
#include "media/MediaTypes.h"
#include "platform/NativeAppWindow.h"
#include "platform/PlatformApplicationServices.h"
#include "platform/PlatformCapabilities.h"
#include "platform/PlatformPaths.h"
#include "platform/PlatformPlaybackRuntime.h"
#include "platform/PlatformProcess.h"
#include "platform/PlatformStartup.h"
#include "platform/ScreenSaverInhibitor.h"
#include "player/MpvVideoItem.h"
#include "player/PlayerController.h"
#include "provider/Provider.h"
#include "provider/ProviderRegistry.h"
#if defined(SPOOL_ANDROID) || defined(JELLYFIN_NATIVE_WEBOS)
#include "platform/UpdateController.h"
#endif
#if defined(SPOOL_ANDROID)
#include <QJniObject>
#endif

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QLoggingCategory>

#if defined(JELLYFIN_NATIVE_WEBOS)
#include "platform/webos/WebOSDeviceName.h"
#endif
#include <QMessageLogContext>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QPointer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQmlIncubationController>
#include <QQmlPropertyMap>
#include <QQuickGraphicsConfiguration>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScreen>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QThread>
#include <QTimer>
#include <qqml.h>

#include <atomic>
#include <clocale>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif
#ifdef SPOOL_ANDROID
#include <android/log.h>
#endif

namespace {

// The window's stock incubation controller advances async QML construction
// ~5 ms per frame, so a page whose creation costs ~200 ms of CPU takes
// 500-700 ms of wall time to instantiate. A timer-driven controller with a
// larger slice cuts cold page construction and idle prewarming to roughly the
// CPU cost, at worst lengthening frames by the slice while incubating.
class BoostedIncubationController final : public QObject, public QQmlIncubationController {
public:
    explicit BoostedIncubationController(QObject *parent)
        : QObject(parent)
    {
        m_timer.setInterval(16);
        m_timer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&m_timer, &QTimer::timeout, this, [this] { incubateFor(kSliceMs); });
    }

protected:
    void incubatingObjectCountChanged(int count) override
    {
        if (count > 0)
            m_timer.start();
        else
            m_timer.stop();
    }

private:
    static constexpr int kSliceMs = 12;
    QTimer m_timer;
};

constexpr auto kAppId = "com.sachk.spool";
constexpr auto kAppVersion = JELLYFIN_VERSION;

FILE *g_logFile = nullptr;
QByteArray g_logPath;
QElapsedTimer g_startupTimer;
std::mutex g_logMutex;

void writeStandardError(const QByteArray& line)
{
#ifdef Q_OS_WIN
    // GUI-subsystem processes do not reliably have a CRT stderr descriptor.
    // Write through the Win32 handle so a missing console cannot trigger the
    // UCRT invalid-parameter fast-fail.
    const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    if (!handle || handle == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(handle, line.constData(), static_cast<DWORD>(line.size()), &written, nullptr);
#elif defined(SPOOL_ANDROID)
    // Android discards a process's stderr, so logcat is the only place these
    // lines can be read from a device or an emulator.
    QByteArray message = line;
    while (message.endsWith('\n'))
        message.chop(1);
    __android_log_write(ANDROID_LOG_INFO, "Spool", message.constData());
#else
    fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stderr);
#endif
}

FILE *openRotatedLogFile(const QByteArray& path)
{
    JellyfinNative::rotateLogFile(path.constData());
    return fopen(path.constData(), "w");
}

FILE *openAppLogFile(const QString& appRootPath)
{
    const QByteArray fileName = QFile::encodeName(JellyfinNative::appLogFileName());
    for (const QString& directory : JellyfinNative::appLogDirectories(appRootPath)) {
        if (directory.isEmpty())
            continue;
        QDir().mkpath(directory);
        QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        const QByteArray encodedDirectory = QFile::encodeName(directory);
        const QByteArray path = QFile::encodeName(QDir(directory).filePath(QString::fromUtf8(fileName)));
        if (FILE *file = openRotatedLogFile(path)) {
            QFile::setPermissions(QString::fromLocal8Bit(path), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            g_logPath = path;
            qputenv("JELLYFIN_NATIVE_LOG_DIR", encodedDirectory);
            return file;
        }
    }
    return nullptr;
}

void configurePersistentStartupCaches(const QString& cacheRoot)
{
    const QString qtShaderCache = QDir(cacheRoot).filePath(QStringLiteral("qtshadercache"));
    const QString qmlDiskCache = QDir(cacheRoot).filePath(QStringLiteral("qmlcache"));
    QDir().mkpath(qtShaderCache);
    QDir().mkpath(qmlDiskCache);

    // Qt snapshots these locations during platform and renderer setup. Keep
    // shader caches in app-owned persistent storage and never clear them.
    qputenv("XDG_CACHE_HOME", QFile::encodeName(cacheRoot));
    qputenv("QT_SHADER_CACHE_PATH", QFile::encodeName(qtShaderCache));
    qputenv("QML_DISK_CACHE_PATH", QFile::encodeName(qmlDiskCache));
}

void configurePersistentRhiPipelineCache(QQuickWindow& window, const QString& cacheRoot)
{
    const QString rhiCacheDir = QDir(cacheRoot).filePath(QStringLiteral("rhi-pipeline-cache"));
    QDir().mkpath(rhiCacheDir);

    const QString cacheFile = QDir(rhiCacheDir).filePath(QStringLiteral("qt-rhi-pipeline-cache.bin"));
    QQuickGraphicsConfiguration graphicsConfig;
    graphicsConfig.setAutomaticPipelineCache(true);
    if (QFileInfo::exists(cacheFile))
        graphicsConfig.setPipelineCacheLoadFile(cacheFile);
    graphicsConfig.setPipelineCacheSaveFile(cacheFile);
    window.setGraphicsConfiguration(graphicsConfig);
}

void logLine(const char *fmt, ...)
{
    const std::lock_guard lock(g_logMutex);
    const long long elapsedMs = g_startupTimer.isValid() ? static_cast<long long>(g_startupTimer.elapsed()) : 0;

    va_list ap;
    va_start(ap, fmt);
    const QByteArray message = QString::vasprintf(fmt, ap).toUtf8();
    va_end(ap);
    const QByteArray line = QByteArrayLiteral("[") + QByteArray::number(elapsedMs).rightJustified(7)
        + QByteArrayLiteral(" ms] ") + message + '\n';

    if (g_logFile) {
        fwrite(line.constData(), 1, static_cast<size_t>(line.size()), g_logFile);
        fflush(g_logFile);
    }
    writeStandardError(line);
}

void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (type == QtWarningMsg && !qEnvironmentVariableIsSet("JELLYFIN_NATIVE_VERBOSE_QT")) {
        const QString category = context.category ? QString::fromLatin1(context.category) : QString();
        if (message.startsWith(QStringLiteral("Detected locale \"C\""))
            || (category == QStringLiteral("qt.qpa.wayland")
                && message.contains(QStringLiteral("\"wl-shell\" is a deprecated shell extension"))))
            return;
    }

    const char *level = "debug";
    switch (type) {
    case QtDebugMsg:
        level = "debug";
        break;
    case QtInfoMsg:
        level = "info";
        break;
    case QtWarningMsg:
        level = "warn";
        break;
    case QtCriticalMsg:
        level = "crit";
        break;
    case QtFatalMsg:
        level = "fatal";
        break;
    }

    const QByteArray local = JellyfinNative::sanitizedLogMessage(message).toLocal8Bit();
    if (context.category && context.category[0])
        logLine("[qt:%s] %s: %s", level, context.category, local.constData());
    else
        logLine("[qt:%s] %s", level, local.constData());

    if (type == QtFatalMsg)
        abort();
}

void logQmlWarnings(const QList<QQmlError>& warnings)
{
    for (const QQmlError& warning : warnings)
        logLine("[qml] %s", qPrintable(warning.toString()));
}

#if !defined(JELLYFIN_NATIVE_WEBOS) && !defined(SPOOL_ANDROID)
QIcon applicationIcon(bool playerSelected)
{
    const QString variant = playerSelected ? QStringLiteral("spool-film") : QStringLiteral("spool");
    return QIcon(QStringLiteral(":/icons/%1.svg").arg(variant));
}
#endif

bool registerBundledFonts(const QString& appRootPath)
{
    static constexpr const char *fontFiles[] = {
        "AtkinsonHyperlegible-Bold.otf",
        "AtkinsonHyperlegible-Regular.otf",
        "IBMPlexSans-Variable.ttf",
        "PTRootUI-Variable.ttf",
        "MaterialIcons-Regular.ttf",
    };
    const QDir fontsDirectory(JellyfinNative::bundledFontsPath(appRootPath));
    for (const char *fileName : fontFiles) {
        const QString path = fontsDirectory.filePath(QString::fromLatin1(fileName));
        if (!QFileInfo::exists(path)) {
            logLine("font registration failed: missing bundled font: %s", qPrintable(path));
            return false;
        }
        if (QFontDatabase::addApplicationFont(path) < 0) {
            logLine("font registration failed: unloadable bundled font: %s", qPrintable(path));
            return false;
        }
    }
    return true;
}

// The size the system's launch frame drew the mark at. Android is the only
// platform that puts one up before Qt, and it draws the mark at an explicit dp
// size out of tools/manifests/splash.json, which CMake bakes in here. Every
// other platform sizes the mark against the viewport instead.
double splashCoreWidthDp()
{
#if defined(SPOOL_SPLASH_CORE_WIDTH_DP_PHONE)
    // The dp the platform's own launch frame drew the mark at. One package
    // ships both launch screens under the -television resource qualifier, so
    // this has to make the same choice the resource system just made or Qt's
    // first frame lands on different pixels than the frame it replaces.
    return static_cast<double>(
        JellyfinNative::platformCapabilities().isTV ? SPOOL_SPLASH_CORE_WIDTH_DP_TV : SPOOL_SPLASH_CORE_WIDTH_DP_PHONE);
#else
    return 0.0;
#endif
}

// webOS reads its launch image off disk, where appinfo.json already names it;
// everywhere else it is in the binary.
QUrl splashImageUrl(const QString& appRootPath)
{
#if defined(JELLYFIN_NATIVE_WEBOS)
    return QUrl::fromLocalFile(QDir(appRootPath).filePath(QStringLiteral("splash-core.png")));
#else
    Q_UNUSED(appRootPath);
    return QUrl(QStringLiteral("qrc:/startup/splash-core.png"));
#endif
}

// QML units per Android dp. Qt's own scaling already folds part of the
// display's density into the coordinate system, so divide it back out rather
// than assuming which of the two conventions this Qt build uses.
//
// Getting this wrong is not a rounding error: the mark is sized in dp to match
// the frame the system already has on screen, and falling through to the
// viewport rule instead drew it at twice the size, so the handover jumped.
// Hence the second opinion and the log line -- a zero here has to be visible.
double splashPixelsPerDp()
{
#if defined(SPOOL_ANDROID)
    // Answered once. Asking twice gave two different answers: the second call
    // failed the platform lookup and fell back to a logical-DPI reading five
    // times too small, so whichever caller ran second got a launch screen
    // sized from a number the first caller never saw.
    static const double cached = [] {
        double density = 0.0;
        const char *source = "none";
        const QJniObject context = QNativeInterface::QAndroidApplication::context();
        if (context.isValid()) {
            const QJniObject resources = context.callObjectMethod("getResources", "()Landroid/content/res/Resources;");
            const QJniObject metrics = resources.isValid()
                ? resources.callObjectMethod("getDisplayMetrics", "()Landroid/util/DisplayMetrics;")
                : QJniObject();
            if (metrics.isValid()) {
                density = static_cast<double>(metrics.getField<jfloat>("density"));
                source = "displayMetrics";
            }
        }
        const QScreen *screen = QGuiApplication::primaryScreen();
        // Android's dp is defined against 160 dpi, so Qt's own reading of the
        // display answers the same question when the platform one does not.
        if (density <= 0.0 && screen) {
            density = screen->logicalDotsPerInch() / 160.0;
            source = "logicalDpi";
        }
        const double ratio = screen ? screen->devicePixelRatio() : 1.0;
        const double perDp = density > 0.0 && ratio > 0.0 ? density / ratio : density;
        logLine("splash: density=%.3f (%s) dpr=%.3f pixelsPerDp=%.3f", density, source, ratio, perDp);
        return perDp > 0.0 ? perDp : 0.0;
    }();
    return cached;
#else
    return 0.0;
#endif
}

} // namespace

int main(int argc, char **argv)
{
#ifdef Q_OS_UNIX
    umask(S_IRWXG | S_IRWXO);
#endif
    const JellyfinNative::ProcessStartupTiming processStartupTiming = JellyfinNative::captureProcessStartupTiming();
    g_startupTimer.start();
    QElapsedTimer& startupTimer = g_startupTimer;
    bool launchTest = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("Spool for Jellyfin %s\n", kAppVersion);
            return 0;
        }
        if (strcmp(argv[i], "--launch-test") == 0)
            launchTest = true;
    }

    const QString appRootPath = JellyfinNative::resolveAppRoot(argv[0]);
    if (appRootPath.isEmpty())
        return 1;

    g_logFile = openAppLogFile(appRootPath);
    logLine("%s starting", kAppId);
    logLine("startup: exec_to_main_ms=%lld static_init_ms=%.2f",
        static_cast<long long>(processStartupTiming.execToMainMs), processStartupTiming.staticInitializationMs);
    if (!g_logPath.isEmpty())
        logLine("log file: %s", g_logPath.constData());

    // libmpv parses option strings (and many internal numeric values) with the
    // C locale assumption — under any other LC_NUMERIC playback fails to start
    // because option parsing rejects floating-point arguments. Force LC_NUMERIC
    // to C for both this process and any inherited child env so the user does
    // not have to set LC_NUMERIC=C themselves.
    setlocale(LC_NUMERIC, "C");
    qputenv("LC_NUMERIC", QByteArrayLiteral("C"));
    if (setlocale(LC_CTYPE, "C.UTF-8")) {
        qputenv("LANG", QByteArrayLiteral("C.UTF-8"));
        qputenv("LC_CTYPE", QByteArrayLiteral("C.UTF-8"));
    } else if (setlocale(LC_CTYPE, "en_US.UTF-8")) {
        qputenv("LANG", QByteArrayLiteral("en_US.UTF-8"));
        qputenv("LC_CTYPE", QByteArrayLiteral("en_US.UTF-8"));
    }

    if (!JellyfinNative::configurePlatformEnvironment(appRootPath))
        return 1;
    if (!qgetenv("QSG_RENDER_LOOP").isEmpty())
        logLine("QSG_RENDER_LOOP=%s", qgetenv("QSG_RENDER_LOOP").constData());

    const QString cachePath = JellyfinNative::startupCacheRoot(appRootPath);
    configurePersistentStartupCaches(cachePath);

    logLine("app root: %s", qPrintable(appRootPath));
    logLine("QT_QPA_PLATFORM=%s", qgetenv("QT_QPA_PLATFORM").constData());
    logLine("QT_PLUGIN_PATH=%s", qgetenv("QT_PLUGIN_PATH").constData());
    logLine("QML2_IMPORT_PATH=%s", qgetenv("QML2_IMPORT_PATH").constData());
    if (!qgetenv("QT_IM_MODULE").isEmpty())
        logLine("QT_IM_MODULE=%s", qgetenv("QT_IM_MODULE").constData());

    qInstallMessageHandler(qtMessageHandler);
    QLoggingCategory::setFilterRules(QStringLiteral("qt.*.debug=false\nqt.*.info=false"));

    // Production playback needs OpenGL for MpvVideoItem's FBO. Launch tests
    // validate the QML scene on headless runners, where no OpenGL adapter is
    // guaranteed, so use Qt Quick's deterministic software renderer.
    QQuickWindow::setGraphicsApi(launchTest ? QSGRendererInterface::Software : QSGRendererInterface::OpenGL);

    QSurfaceFormat::setDefaultFormat(JellyfinNative::platformSurfaceFormat());

    logLine("startup: constructing QGuiApplication");
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Spool for Jellyfin"));
    app.setApplicationVersion(QString::fromLatin1(kAppVersion));
    app.setOrganizationName(QStringLiteral("spool-jellyfin"));
    app.setApplicationDisplayName(QStringLiteral("Spool for Jellyfin"));
    JellyfinNative::TerminationSignalHandler terminationSignals(app);
    logLine("startup: QGuiApplication constructed");

    // Put a native surface on screen at the first valid opportunity. QWindow
    // and scene-graph setup must remain on the GUI thread; everything below
    // this point can overlap the render thread's first-frame work instead of
    // delaying it.
    JellyfinNative::InputLatencyMonitor inputLatencyMonitor;
    JellyfinNative::NativeAppWindow window(QString::fromLatin1(kAppId));
    // The launch screen. Everything that draws it -- the frame put up before
    // the shell exists, the shell's own overlay, the slow-start page -- reads
    // these, so all three land on the same pixels and the handover from the
    // system's launch frame shows nothing.
    window.rootContext()->setContextProperty(QStringLiteral("startupSplashImageUrl"), splashImageUrl(appRootPath));
    window.rootContext()->setContextProperty(
        QStringLiteral("startupSplashCoreAspect"), static_cast<double>(SPOOL_SPLASH_CORE_ASPECT));
    window.rootContext()->setContextProperty(
        QStringLiteral("startupSplashCoreWidthFraction"), static_cast<double>(SPOOL_SPLASH_CORE_WIDTH_FRACTION));
    window.rootContext()->setContextProperty(QStringLiteral("startupSplashCoreWidthDp"), splashCoreWidthDp());
    window.rootContext()->setContextProperty(QStringLiteral("startupSplashPixelsPerDp"), splashPixelsPerDp());
    JellyfinNative::configurePlatformWindow(window);
    inputLatencyMonitor.attachWindow(&window);
    window.setInputLatencyMonitor(&inputLatencyMonitor);
    const auto directSingleShot = static_cast<Qt::ConnectionType>(Qt::DirectConnection | Qt::SingleShotConnection);
    const auto traceFirstFrameSignal = [&window, &startupTimer, directSingleShot](auto signal, const char *name) {
        QObject::connect(
            &window, signal, &window,
            [&window, &startupTimer, name] {
                const qint64 elapsedMs = startupTimer.elapsed();
                QMetaObject::invokeMethod(
                    &window, [elapsedMs, name] { logLine("startup: first frame %s at %lld ms", name, elapsedMs); },
                    Qt::QueuedConnection);
            },
            directSingleShot);
    };
    traceFirstFrameSignal(&QQuickWindow::beforeFrameBegin, "begin");
    traceFirstFrameSignal(&QQuickWindow::beforeSynchronizing, "sync_begin");
    traceFirstFrameSignal(&QQuickWindow::afterSynchronizing, "sync_end");
    traceFirstFrameSignal(&QQuickWindow::beforeRendering, "render_begin");
    traceFirstFrameSignal(&QQuickWindow::afterRendering, "render_end");
    traceFirstFrameSignal(&QQuickWindow::frameSwapped, "swapped");
    traceFirstFrameSignal(&QQuickWindow::afterFrameEnd, "end");
    configurePersistentRhiPipelineCache(window, cachePath);
#if !defined(SPOOL_ANDROID)
    window.setSource(QUrl(QStringLiteral("qrc:/startup/StartupSplash.qml")));
    if (window.status() == QQuickView::Error) {
        logQmlWarnings(window.errors());
        return 1;
    }
    if (!window.prepareForUiSurface()) {
        logLine("failed to initialize the native UI surface");
        return 1;
    }
    logLine("startup: prepareForUiSurface completed in %lld ms", static_cast<long long>(startupTimer.elapsed()));
#endif

    if (!registerBundledFonts(appRootPath))
        return 1;
#if !defined(JELLYFIN_NATIVE_WEBOS) && !defined(SPOOL_ANDROID)
    const QIcon defaultApplicationIcon = applicationIcon(false);
    const QIcon playerApplicationIcon = applicationIcon(true);
    app.setWindowIcon(defaultApplicationIcon);
    app.setDesktopFileName(QStringLiteral("com.sachk.spool"));
#endif
    const auto& capabilities = JellyfinNative::platformCapabilities();

    const QString diagnosticsRoot = qEnvironmentVariableIsSet("JELLYFIN_DIAGNOSTICS_DIR")
        ? QString::fromLocal8Bit(qgetenv("JELLYFIN_DIAGNOSTICS_DIR"))
        : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/diagnostics");
    JellyfinNative::Diagnostics::initialize(QString::fromLatin1(kAppId), diagnosticsRoot);
    JellyfinNative::Diagnostics::EventLoopWatchdog eventLoopWatchdog(&app);

    const QStringList arguments = app.arguments();
    if (arguments.contains(QStringLiteral("--diagnose-and-exit"))
        || arguments.contains(QStringLiteral("--dump-diagnostics"))) {
        JellyfinNative::Diagnostics::dumpDiagnostics(QStringLiteral("command-line"));
        JellyfinNative::Diagnostics::shutdown();
        return 0;
    }
    if (qEnvironmentVariableIntValue("JELLYFIN_DIAGNOSTICS_BLOCK_GUI_MS") > 0) {
        const int blockMs = qEnvironmentVariableIntValue("JELLYFIN_DIAGNOSTICS_BLOCK_GUI_MS");
        QTimer::singleShot(1000, &app, [blockMs]() {
            JellyfinNative::Diagnostics::logEvent(QStringLiteral("simulation"), QStringLiteral("block_gui_begin"),
                { { QStringLiteral("durationMs"), blockMs } });
            QThread::msleep(static_cast<unsigned long>(blockMs));
            JellyfinNative::Diagnostics::logEvent(QStringLiteral("simulation"), QStringLiteral("block_gui_end"));
        });
    }

    const JellyfinNative::MemoryBudget memoryBudget = JellyfinNative::MemoryBudget::detect();
    window.setSystemMemoryBytes(memoryBudget.memTotalBytes);
    logLine("memory budget: memTotal=%lld networkDisk=%lld qmlImageDisk=%lld artworkBytes=%d demuxer=%s/%s",
        static_cast<long long>(memoryBudget.memTotalBytes), static_cast<long long>(memoryBudget.networkDiskCacheBytes),
        static_cast<long long>(memoryBudget.qmlImageDiskCacheBytes), memoryBudget.artworkByteCacheBytes,
        memoryBudget.mpvDemuxerMaxBytes.constData(), memoryBudget.mpvDemuxerMaxBackBytes.constData());

    auto *networkAccessManager = new QNetworkAccessManager(&app);
    auto *diskCache = new QNetworkDiskCache(networkAccessManager);
    const QString qmlImageCachePath = cachePath + QStringLiteral("/qml-image-cache");
    QDir().mkpath(cachePath);
    diskCache->setCacheDirectory(cachePath + QStringLiteral("/network-cache"));
    diskCache->setMaximumCacheSize(memoryBudget.networkDiskCacheBytes);
    networkAccessManager->setCache(diskCache);
    QObject *platformUpdateController = nullptr;
#if defined(SPOOL_ANDROID) || defined(JELLYFIN_NATIVE_WEBOS)
    // QML binds before the shell exists. Android waits for its settings;
    // the webOS experiment starts its unconditional check in the event loop.
    auto updateController = std::make_unique<JellyfinNative::UpdateController>(networkAccessManager, cachePath);
    platformUpdateController = updateController.get();
#endif

    JellyfinNative::DatabaseManager database;
    QObject::connect(
        &database, &JellyfinNative::DatabaseManager::initializationFailed, &app, [&app](const QString& message) {
            logLine("database initialization failed: %s", qPrintable(message));
            app.exit(1);
        });
    JellyfinNative::SystemPerformanceMonitor systemPerformanceMonitor;
    systemPerformanceMonitor.setAudioDecodeCpuTimeProvider(
        [] { return JellyfinNative::platformAudioDecodeCpuTimeNs(); });
    // Start the SQLite worker before constructing the controllers. Device
    // identity and session reads are awaited after the first frame.
    {
        JellyfinNative::Diagnostics::Phase phase(QStringLiteral("startup"), QStringLiteral("database_initialize"));
        const QString databasePath = JellyfinNative::persistentDataRoot() + QStringLiteral("/cache.sqlite");
        if (!database.initialize(databasePath))
            return 1;
    }

    JellyfinNative::TlsTrustController tlsTrust;
    // One provider, chosen here until the first-run picker exists. It
    // outlives the player and the app controller, which hold its parts by
    // pointer, so it is declared before them. The app only ever sees it as
    // a Provider.
    JellyfinNative::ProviderRegistry providers;
    auto jellyfin = std::make_unique<JellyfinNative::JellyfinProvider>(JellyfinNative::JellyfinProviderContext {
        networkAccessManager, &tlsTrust, &database, capabilities.deviceName, QString::fromLatin1(kAppVersion) });
    providers.add(jellyfin.get());
    providers.setActive(jellyfin.get());
    JellyfinNative::Provider *provider = providers.active();

    const JellyfinNative::CpuTopology cpuTopology = JellyfinNative::detectCpuTopology();
    logLine("artwork: cpu logical=%d physical=%d smt=%s source=%s decodeThreads=%d", cpuTopology.logicalCpus,
        cpuTopology.physicalCores, cpuTopology.smtDetected ? "true" : "false", qPrintable(cpuTopology.source),
        cpuTopology.artworkDecodeThreads);
    auto artworkService = std::make_unique<JellyfinNative::ArtworkService>(
        qmlImageCachePath + QStringLiteral("/artwork"), memoryBudget.qmlImageDiskCacheBytes,
        memoryBudget.artworkByteCacheBytes, cpuTopology.artworkDecodeThreads, &tlsTrust);
    artworkService->setUiWidth(window.width());
    artworkService->setSource(provider->artwork());

    auto player = std::make_unique<JellyfinNative::PlayerController>(
        &window, provider->playback(), &tlsTrust, JellyfinNative::bundledFontsPath(appRootPath));
    player->setDemuxerBudget(memoryBudget.mpvDemuxerMaxBytes, memoryBudget.mpvDemuxerMaxBackBytes);
    JellyfinNative::ScreenSaverInhibitor screenSaverInhibitor;
    const auto updateScreenSaver = [&screenSaverInhibitor, player = player.get()] {
        screenSaverInhibitor.setInhibited(JellyfinNative::screenSaverShouldBeInhibited(
            player && player->sessionActive(), player && player->paused()));
    };
    QObject::connect(player.get(), &JellyfinNative::PlayerController::playbackStateChanged, &app, updateScreenSaver);
    QObject::connect(player.get(), &JellyfinNative::PlayerController::sessionActiveChanged, &app, updateScreenSaver);
#if !defined(JELLYFIN_NATIVE_WEBOS) && !defined(SPOOL_ANDROID)
    const auto updateApplicationIcon
        = [&app, &window, player = player.get(), &defaultApplicationIcon, &playerApplicationIcon] {
              const QIcon& icon = player->sessionActive() ? playerApplicationIcon : defaultApplicationIcon;
              app.setWindowIcon(icon);
              window.setIcon(icon);
          };
    QObject::connect(
        player.get(), &JellyfinNative::PlayerController::sessionActiveChanged, &app, updateApplicationIcon);
    updateApplicationIcon();
#endif
    auto controller
        = std::make_unique<JellyfinNative::AppController>(&database, provider, artworkService.get(), player.get());
    controller->settings()->setProviderCapabilities(providers.capabilities());
#if defined(SPOOL_ANDROID) || defined(JELLYFIN_NATIVE_WEBOS)
    // Settings own the update preference; platform installers own installation.
    QObject::connect(controller->settings(), &JellyfinNative::SettingsController::automaticUpdatesChanged,
        updateController.get(),
        [updater = updateController.get()](bool enabled) { updater->setAutomaticUpdatesEnabled(enabled); });
#endif
    QObject::connect(controller.get(), &JellyfinNative::AppController::clearLogsRequested, &app, [appRootPath]() {
        const std::lock_guard lock(g_logMutex);
        if (g_logFile) {
            fclose(g_logFile);
            g_logFile = nullptr;
        }
        const QByteArray fileName = QFile::encodeName(JellyfinNative::appLogFileName());
        for (const QString& directory : JellyfinNative::appLogDirectories(appRootPath)) {
            if (directory.isEmpty())
                continue;
            const QString path = QDir(directory).filePath(QString::fromUtf8(fileName));
            QFile::remove(path);
            QFile::remove(path + QStringLiteral(".1"));
            QFile::remove(path + QStringLiteral(".2"));
        }
        g_logFile = openAppLogFile(appRootPath);
    });
    // A desktop close event arrives while the scene graph is still rendering.
    // Tear down here so the mpv render-context handoff completes immediately;
    // aboutToQuit is too late because the window no longer produces frames.
    QObject::connect(
        &window, &JellyfinNative::NativeAppWindow::closeRequested, controller.get(),
        [controller = controller.get()]() {
            logLine("window close requested: stopping controllers");
            controller->shutdown();
        },
        Qt::DirectConnection);

    // Shutdown sequence (runs while the event loop and scene graph are still
    // alive, before any of the unique_ptrs below get destructed):
    //   1. Tear mpv down — stops audio/decode threads and frees the render
    //      context. mpv_terminate_destroy joins everything synchronously.
    //   2. Clear the QQuickView's source so QML items unbind from the
    //      `appController` / `nativeWindow` context properties before those
    //      objects are destroyed. Otherwise the bindings keep evaluating
    //      against null pointers and emit a flood of "Cannot read property
    //      'X' of null" warnings during the unwind.
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [controller = controller.get(), &window]() {
        JellyfinNative::Diagnostics::setInstanceState(QStringLiteral("shutting_down"));
        JellyfinNative::Diagnostics::Phase shutdownPhase(QStringLiteral("shutdown"), QStringLiteral("aboutToQuit"));
        logLine("aboutToQuit: stopping controllers");
        if (qEnvironmentVariableIntValue("JELLYFIN_DIAGNOSTICS_SHUTDOWN_HANG_MS") > 0) {
            const int hangMs = qEnvironmentVariableIntValue("JELLYFIN_DIAGNOSTICS_SHUTDOWN_HANG_MS");
            JellyfinNative::Diagnostics::logEvent(QStringLiteral("simulation"), QStringLiteral("shutdown_hang_begin"),
                { { QStringLiteral("durationMs"), hangMs } });
            QThread::msleep(static_cast<unsigned long>(hangMs));
            JellyfinNative::Diagnostics::logEvent(QStringLiteral("simulation"), QStringLiteral("shutdown_hang_end"));
        }
        {
            JellyfinNative::Diagnostics::Phase phase(QStringLiteral("shutdown"), QStringLiteral("controller_shutdown"));
            controller->shutdown();
        }
        logLine("aboutToQuit: clearing QML source");
        {
            JellyfinNative::Diagnostics::Phase phase(QStringLiteral("shutdown"), QStringLiteral("clear_qml_source"));
            window.setSource(QUrl());
        }
        logLine("aboutToQuit: QML source cleared");
    });

    // Parented to the engine so it outlives every incubator and dies with it.
    window.engine()->setIncubationController(new BoostedIncubationController(window.engine()));
    window.engine()->addImageProvider(
        QStringLiteral("artwork"), new JellyfinNative::ArtworkImageProvider(artworkService.get()));
    window.engine()->addImageProvider(QStringLiteral("mpv-overlay"), window.createOverlayImageProvider());
#if !defined(JELLYFIN_NATIVE_WEBOS) && !defined(SPOOL_ANDROID)
    window.engine()->addImportPath(appRootPath + QStringLiteral("/qt-qml"));
#endif
    QObject::connect(window.engine(), &QQmlEngine::warnings, &logQmlWarnings);
    QObject::connect(&window, &QQuickView::statusChanged,
        [](QQuickView::Status status) { logLine("view status changed: %d", static_cast<int>(status)); });
    auto localization = std::make_unique<JellyfinNative::LocalizationManager>();
    localization->attachToEngine(window.engine());
    provider->setLocale(localization->bcp47Locale());
    QObject::connect(localization.get(), &JellyfinNative::LocalizationManager::localeChanged, provider,
        [provider, loc = localization.get()]() { provider->setLocale(loc->bcp47Locale()); });
    auto router = std::make_unique<JellyfinNative::RouterController>();
    JellyfinNative::ApplicationHooks applicationHooks;
    applicationHooks.player = player.get();
    applicationHooks.settings = controller->settings();
    applicationHooks.memoryPressure
        = [controller = controller.get()](const QString& level) { controller->onMemoryPressure(level); };
    QObject::connect(controller.get(), &JellyfinNative::AppController::aggressiveMemoryPressure, &applicationHooks,
        &JellyfinNative::ApplicationHooks::aggressiveMemoryPressure);
    QObject::connect(controller.get(), &JellyfinNative::AppController::diagnosticsReportSaved, &applicationHooks,
        &JellyfinNative::ApplicationHooks::diagnosticsReportSaved);
    QObject::connect(&applicationHooks, &JellyfinNative::ApplicationHooks::toastRequested, controller.get(),
        &JellyfinNative::AppController::toastMessage);
    JellyfinNative::PlatformApplicationServices platformServices(app, window, applicationHooks, *router);
    platformServices.start();
    QQmlPropertyMap *platformInfo = QQmlPropertyMap::create(&app);
    platformInfo->insert(QStringLiteral("isTV"), capabilities.isTV);
    platformInfo->insert(QStringLiteral("isWebOS"), capabilities.isWebOS);
    platformInfo->insert(QStringLiteral("isAndroid"), capabilities.isAndroid);
    platformInfo->insert(QStringLiteral("isMobile"), capabilities.isMobile);
#ifdef TOUCHSCREEN
    platformInfo->insert(QStringLiteral("touchscreen"), !capabilities.isTV);
#else
    platformInfo->insert(QStringLiteral("touchscreen"), false);
#endif
    platformInfo->insert(QStringLiteral("hasSystemFonts"), capabilities.hasSystemFonts);
    platformInfo->insert(QStringLiteral("hasDesktopPointer"), capabilities.hasDesktopPointer);
    platformInfo->insert(QStringLiteral("hasPointer"), capabilities.hasPointer);
    platformInfo->insert(QStringLiteral("supportsMpvConfig"), capabilities.supportsMpvConfig);
    platformInfo->insert(QStringLiteral("usesPerOutputAudioDelay"), capabilities.usesPerOutputAudioDelay);
    platformInfo->insert(QStringLiteral("deviceName"), capabilities.deviceName);
    platformInfo->insert(QStringLiteral("updateController"), QVariant::fromValue(platformUpdateController));
    platformInfo->insert(QStringLiteral("rendererName"), capabilities.rendererName);
    // The launch screen's geometry, alongside the context properties the
    // pre-shell frame reads. The shell reaches it through this singleton
    // instead, because that is what reliably resolves from a component the
    // shell loads rather than one the view loads itself.
    platformInfo->insert(QStringLiteral("splashPixelsPerDp"), splashPixelsPerDp());
    platformInfo->insert(QStringLiteral("splashCoreWidthDp"), splashCoreWidthDp());
    platformInfo->insert(
        QStringLiteral("splashCoreWidthFraction"), static_cast<double>(SPOOL_SPLASH_CORE_WIDTH_FRACTION));
    platformInfo->insert(QStringLiteral("splashCoreAspect"), static_cast<double>(SPOOL_SPLASH_CORE_ASPECT));
    platformInfo->insert(QStringLiteral("splashImageUrl"), splashImageUrl(appRootPath));
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "App", controller.get());
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "ProviderCapabilities", providers.capabilities());
    provider->registerQmlSingletons();
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "Art", artworkService.get());
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "Browse", controller->browse());
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "Home", controller->home());
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "Content", controller->content());
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "ItemState", controller->itemState());
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "Search", controller->search());
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "Libraries", controller->libraries());
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "TlsTrust", &tlsTrust);
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "Settings", controller->settings());
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "Player", controller->player());
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "PlayQueue", controller->playQueue());
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "Router", router.get());
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "NativeWindow", &window);
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "InputLatency", &inputLatencyMonitor);
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "SystemPerformance", &systemPerformanceMonitor);
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "I18n", localization.get());
    qmlRegisterSingletonInstance("JellyfinWebOS", 1, 0, "Platform", platformInfo);
    qmlRegisterType<JellyfinNative::MpvVideoItem>("JellyfinWebOS", 1, 0, "MpvVideoItem");
    // Start asynchronous device, settings, account, and discovery reads before
    // QML construction. A sole saved account is resolved before routing begins.
    controller->initialize();

    QObject::connect(
        &app, &QCoreApplication::aboutToQuit, router.get(), [router = router.get()] { router->markCleanShutdown(); });

    {
        JellyfinNative::Diagnostics::Phase phase(QStringLiteral("startup"), QStringLiteral("load_qml"));
        // Load through the module registry so AOT QML units are used and
        // constrained devices do not retain compiler buffers.
        window.loadFromModule("JellyfinWebOS", "Main");
    }
    if (window.status() == QQuickView::Error) {
        logQmlWarnings(window.errors());
        return 1;
    }
#if defined(SPOOL_ANDROID)
    if (!window.prepareForUiSurface()) {
        logLine("failed to initialize the native UI surface");
        return 1;
    }
    // Hand the launch screen over the moment Qt has actually drawn one. The
    // shell's first frame is the same picture the system frame is showing --
    // same mark, same size, same black -- so this swaps one for the other with
    // nothing on screen changing, and the shell then holds it until a page has
    // painted. Waiting any longer than the first frame only leaves a launch
    // screen Qt could already have taken over.
    QObject::connect(
        &window, &QQuickWindow::frameSwapped, &app, [] { QNativeInterface::QAndroidApplication::hideSplashScreen(); },
        Qt::SingleShotConnection);
#endif
    logLine("startup: QML source loaded in %lld ms", static_cast<long long>(startupTimer.elapsed()));

    // What the launch screen actually resolved. QML's own console does not
    // reach logcat on Android, and this is precisely the platform where the
    // mark has to agree with a frame the system drew, so read it back here.
    QObject::connect(
        &window, &QQuickWindow::frameSwapped, &app,
        [&window] {
            QObject *root = window.rootObject();
            if (!root) {
                logLine("splash: no root object when reading back");
                return;
            }
            QObject *splash = root->findChild<QObject *>(QStringLiteral("startupSplashCore"));
            if (!splash) {
                logLine("splash: launch screen item absent from a tree of %lld objects",
                    static_cast<long long>(root->findChildren<QObject *>().size()));
                return;
            }
            logLine("splash: coreWidth=%.1f pixelsPerDp=%.3f coreWidthDp=%.1f fraction=%.4f viewport=%.0fx%.0f",
                splash->property("coreWidth").toDouble(), splash->property("pixelsPerDp").toDouble(),
                splash->property("coreWidthDp").toDouble(), splash->property("coreWidthFraction").toDouble(),
                splash->property("width").toDouble(), splash->property("height").toDouble());
        },
        static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::SingleShotConnection));

    // Inert unless SPOOL_BENCH names a script. When it does, the app comes up
    // as it always does and is then walked through a set of route switches
    // with what each one cost written out, so page-switch cost is a number in
    // CI rather than an impression.
    JellyfinNative::RenderBenchmarkHooks benchmarkHooks;
    benchmarkHooks.libraries = controller->libraries();
    benchmarkHooks.openLibrary = [controller = controller.get()](int index) { controller->openLibrary(index); };
    benchmarkHooks.outstandingArtworkRequests
        = [artwork = artworkService.get()] { return artwork->outstandingRequests(); };
    benchmarkHooks.artworkDecodeTotals = [artwork = artworkService.get()] {
        const auto totals = artwork->decodeTotals();
        return QVariantMap { { QStringLiteral("decodeMsTotal"), static_cast<double>(totals.decodeNs) / 1000000.0 },
            { QStringLiteral("decodedPixelsTotal"), static_cast<double>(totals.pixels) },
            { QStringLiteral("decodedImagesTotal"), totals.images } };
    };
    benchmarkHooks.forceColdCaches
        = [controller = controller.get()] { controller->onMemoryPressure(QStringLiteral("critical")); };
    if (auto *benchmark = JellyfinNative::RenderBenchmark::createIfRequested(
            std::move(benchmarkHooks), router.get(), &inputLatencyMonitor, &window, &app)) {
        if (controller->initialized()) {
            benchmark->start();
        } else {
            QObject::connect(
                controller.get(), &JellyfinNative::AppController::initializedChanged, benchmark,
                [benchmark, controller = controller.get()] {
                    if (controller->initialized())
                        benchmark->start();
                },
                Qt::SingleShotConnection);
        }
    }

    if (launchTest) {
        QObject::connect(
            &window, &QQuickWindow::frameSwapped, &app,
            [&app, &window] {
                // frameSwapped may be emitted by the render thread. Return to
                // the GUI thread before inspecting QML or beginning shutdown.
                QMetaObject::invokeMethod(
                    &app,
                    [&app, &window] {
                        QObject *rootObject = window.rootObject();
                        QObject *contentLayer = rootObject
                            ? rootObject->findChild<QObject *>(QStringLiteral("shellContentLayer"))
                            : nullptr;
                        QObject *navBar = rootObject
                            ? rootObject->findChild<QObject *>(QStringLiteral("shellNavigationBar"))
                            : nullptr;
                        QObject *routeStack = rootObject
                            ? rootObject->findChild<QObject *>(QStringLiteral("shellRouteStack"))
                            : nullptr;
                        const double contentHeight = contentLayer ? contentLayer->property("height").toDouble() : 0.0;
                        const double contentWidth = contentLayer ? contentLayer->property("width").toDouble() : 0.0;
                        const double navHeight = navBar ? navBar->property("height").toDouble() : 0.0;
                        const double navY = navBar ? navBar->property("y").toDouble() : 0.0;
                        const double routeHeight = routeStack ? routeStack->property("height").toDouble() : 0.0;
                        const double routeWidth = routeStack ? routeStack->property("width").toDouble() : 0.0;
                        const double routeY = routeStack ? routeStack->property("y").toDouble() : 0.0;
                        const bool navAtTop = navY <= 1.0 && routeY + 1.0 >= navHeight;
                        const bool navAtBottom = routeY <= 1.0 && navY + 1.0 >= routeHeight;
                        const bool shellGeometryValid = contentWidth > 0.0 && contentHeight > 0.0 && navHeight >= 0.0
                            && navHeight < contentHeight && routeWidth + 1.0 >= contentWidth && routeHeight > 0.0
                            && routeHeight + navHeight + 1.0 >= contentHeight && routeY >= 0.0 && navY >= 0.0
                            && routeY + routeHeight <= contentHeight + 1.0 && navY + navHeight <= contentHeight + 1.0
                            && (navAtTop || navAtBottom);
                        if (!shellGeometryValid) {
                            logLine("launch test: invalid shell geometry content=%.1fx%.1f nav=%.1f@%.1f "
                                    "route=%.1fx%.1f@%.1f",
                                contentWidth, contentHeight, navHeight, navY, routeWidth, routeHeight, routeY);
                            JellyfinNative::Diagnostics::setInstanceState(QStringLiteral("launch_test_invalid_shell"));
                            app.exit(1);
                            return;
                        }
                        logLine("launch test: application UI rendered");
                        JellyfinNative::Diagnostics::setInstanceState(QStringLiteral("launch_test_rendered"));
                        app.exit(0);
                    },
                    Qt::QueuedConnection);
            },
            directSingleShot);
        QTimer::singleShot(30'000, &app, [&app] {
            logLine("launch test: application UI did not render within 30 seconds");
            app.exit(1);
        });
        window.requestUpdate();
    }

    QTimer::singleShot(1000, router.get(), [router = router.get()] { router->beginSession(false); });

    QTimer::singleShot(0, &window, [&startupTimer]() {
        logLine("startup: first event-loop turn at %lld ms", static_cast<long long>(startupTimer.elapsed()));
        JellyfinNative::Diagnostics::setInstanceState(QStringLiteral("running"));
    });

    logLine("startup: entering event loop at %lld ms", static_cast<long long>(startupTimer.elapsed()));
    const int exitCode = app.exec();
    logLine("app.exec returned: %d", exitCode);
    JellyfinNative::Diagnostics::setInstanceState(
        QStringLiteral("app_exec_returned"), { { QStringLiteral("exitCode"), exitCode } });
    JellyfinNative::Diagnostics::shutdown();
    return exitCode;
}
