#include "provider/ProviderQmlCache.h"
#include "TestMain.h"

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

JELLYFIN_TEST_MAIN("provider-qml-cache")
{
    QGuiApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "fixture directory created");
    const auto source = [&](const QString& name, const QByteArray& bytes) {
        QFile file(directory.filePath(name));
        require(file.open(QIODevice::WriteOnly), "fixture writable");
        require(file.write(bytes) == bytes.size(), "fixture written");
        return QUrl::fromLocalFile(file.fileName());
    };
    const QUrl a = source("A.qml", "import QtQml\nQtObject { Component.onCompleted: probe.objectName = 'created' }");
    const QUrl b = source("B.qml", "import QtQml\nQtObject { required property string label }");
    const QUrl broken = source("Broken.qml", "this is not QML");
    QQmlEngine engine;
    QObject probe;
    engine.rootContext()->setContextProperty("probe", &probe);
    JellyfinNative::ProviderQmlCache cache(&engine);
    int failures = 0;
    bool finished = false;
    QObject::connect(
        &cache, &JellyfinNative::ProviderQmlCache::componentFailed, &app, [&](const QUrl&) { ++failures; });
    QObject::connect(&cache, &JellyfinNative::ProviderQmlCache::finished, &app, [&] { finished = true; });
    cache.addSources({ a, b, broken, a });
    cache.start(30);
    require(cache.retainedCount() == 0, "warming waits for the launch delay");
    waitUntil([&] { return finished; });
    require(cache.retainedCount() == 2 && failures == 1, "unique valid provider components retained across failures");
    require(probe.objectName().isEmpty(), "warming does not instantiate provider code");
    QQmlComponent activated(&engine, a, QQmlComponent::PreferSynchronous);
    std::unique_ptr<QObject> object(activated.create());
    require(object && probe.objectName() == "created", "retained component activates normally on demand");
    cache.clear();
    require(cache.retainedCount() == 0, "memory pressure releases retained components");
    cache.addSources({ a });
    cache.start(0);
    QCoreApplication::processEvents();
    require(cache.retainedCount() == 0, "memory pressure does not restart idle warming");
    return 0;
}
