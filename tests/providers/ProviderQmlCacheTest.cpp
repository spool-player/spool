#include "provider/ProviderQmlCache.h"
#include "TestMain.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QThread>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>

namespace {
void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void waitUntil(const std::function<bool()>& condition)
{
    QElapsedTimer deadline;
    deadline.start();
    while (!condition() && deadline.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(condition(), "asynchronous QML compilation must finish");
}
}

SPOOL_TEST_MAIN("provider-qml-cache")
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QTemporaryDir directory;
    QTemporaryDir installedDirectory;
    require(directory.isValid() && installedDirectory.isValid(), "fixture directories created");
    require(QDir(directory.path()).mkpath("dynamic"), "nested helper directory created");
    const QUrl root = QUrl::fromLocalFile(directory.path() + '/');
    const QUrl installedRoot = QUrl::fromLocalFile(installedDirectory.path() + '/');
    const auto source = [](const QTemporaryDir& package, const QString& name, const QByteArray& bytes) {
        QFile file(package.filePath(name));
        require(file.open(QIODevice::WriteOnly), "fixture writable");
        require(file.write(bytes) == bytes.size(), "fixture written");
        return QUrl::fromLocalFile(file.fileName());
    };
    const QUrl a = source(directory, "A.qml",
        "import QtQml\nQtObject {"
        " property int value: { bindingProbe.objectName = 'evaluated'; return 42 }"
        " Component.onCompleted: probe.objectName = 'created' }");
    source(directory, "Broken.qml", "this is not QML");
    source(directory, "B.qml", "import QtQml\nQtObject { required property string label }");
    const QUrl helper = source(directory, "dynamic/Helper.qml", "import QtQml\nQtObject { property int value: 42 }");
    source(installedDirectory, "Installed.qml", "import QtQml\nQtObject { property int value: 42 }");
    QQmlEngine engine;
    QObject probe;
    QObject bindingProbe;
    engine.rootContext()->setContextProperty("probe", &probe);
    engine.rootContext()->setContextProperty("bindingProbe", &bindingProbe);
    Spool::ProviderQmlCache cache(&engine);
    int failures = 0;
    bool finished = false;
    QSet<QUrl> ready;
    QObject::connect(
        &cache, &Spool::ProviderQmlCache::componentReady, &app, [&](const QUrl& url) { ready.insert(url); });
    QObject::connect(&cache, &Spool::ProviderQmlCache::componentFailed, &app, [&](const QUrl&) { ++failures; });
    QObject::connect(&cache, &Spool::ProviderQmlCache::finished, &app, [&] { finished = true; });
    cache.setPackages({ root, root, QUrl("https://invalid.invalid/package/") });
    cache.start(30);
    require(cache.retainedCount() == 0, "warming waits for the launch delay");
    waitUntil([&] { return finished; });
    require(cache.retainedCount() == 3 && failures == 1, "unique valid provider components retained across failures");
    require(ready.contains(helper), "nested QML absent from manifest UI and imports is also warmed");
    require(probe.objectName().isEmpty() && bindingProbe.objectName().isEmpty(),
        "warming does not instantiate provider code or evaluate bindings");
    QQmlComponent activated(&engine, a, QQmlComponent::PreferSynchronous);
    std::unique_ptr<QObject> object(activated.create());
    require(object && probe.objectName() == "created", "retained component activates normally on demand");
    require(bindingProbe.objectName() == "evaluated", "bindings run only when the component is activated");
    finished = false;
    cache.setPackages({ root, installedRoot });
    waitUntil([&] { return finished; });
    require(cache.retainedCount() == 4 && failures == 1, "newly installed provider UI joins the retained cache");
    cache.setPackages({ installedRoot });
    require(cache.retainedCount() == 1, "obsolete package versions release their retained components");
    cache.clear();
    require(cache.retainedCount() == 0, "memory pressure releases retained components");
    cache.setPackages({ root });
    cache.start(0);
    QCoreApplication::processEvents();
    require(cache.retainedCount() == 0, "memory pressure does not restart idle warming");
    Spool::ProviderQmlCache cancelled(&engine);
    cancelled.setPackages({ installedRoot });
    cancelled.start(0);
    cancelled.clear();
    QCoreApplication::processEvents();
    require(cancelled.retainedCount() == 0, "shutdown before warmup cancels pending compilation");
    Spool::ProviderQmlCache replaced(&engine);
    bool replacedFinished = false;
    bool warmedObsolete = false;
    QObject::connect(&replaced, &Spool::ProviderQmlCache::finished, &app, [&] { replacedFinished = true; });
    QObject::connect(&replaced, &Spool::ProviderQmlCache::componentReady, &app,
        [&](const QUrl& url) { warmedObsolete |= root.isParentOf(url); });
    replaced.setPackages({ root });
    replaced.start(0);
    replaced.setPackages({ installedRoot });
    waitUntil([&] { return replacedFinished; });
    require(replaced.retainedCount() == 1 && !warmedObsolete,
        "replacing a package before enumeration never warms its obsolete version");
    return 0;
}
