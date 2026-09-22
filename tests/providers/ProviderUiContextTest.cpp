#include "provider/ProviderUiContext.h"
#include "ProviderFixture.h"
#include "TestMain.h"
#include "cache/DatabaseManager.h"
#include "provider/ProviderRegistry.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QPersistentModelIndex>
#include <QQmlComponent>
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
void waitFor(const std::function<bool()>& condition)
{
    QElapsedTimer timeout;
    timeout.start();
    while (!condition() && timeout.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(condition(), "provider UI action completes asynchronously");
}
}

JELLYFIN_TEST_MAIN("provider-ui-context")
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    using namespace JellyfinNative;
    QTemporaryDir directory;
    require(directory.isValid(), "isolated UI source database created");
    qputenv("JELLYFIN_CREDENTIAL_STORE_DIR", directory.filePath("credentials").toUtf8());
    DatabaseManager database;
    require(database.initialize(directory.filePath("cache.sqlite")), "durable source index opens");
    const QString installs = directory.filePath("providers");
    require(ProviderPackage::install(ProviderFixture::package(), installs).has_value(), "fixture installs");
    ProviderRegistry registry(&database);
    registry.setInstallDirectory(installs);
    registry.loadModules();
    QCoro::waitFor(registry.restore());
    const QString source = registry.finishSetup({},
        { { "module", "fixture.test" }, { "account", "account" }, { "label", "Fixture" },
            { "configuration", QVariantMap { { "label", "<b>Untrusted title</b>" } } } });
    registry.useAccount(source);
    waitFor([&] { return registry.sourceRunning(source); });
    QQmlEngine engine;
    const auto picker = [&] { return qobject_cast<ProviderUiContext *>(registry.openPicker(source, {})); };
    ProviderUiContext *first = picker();
    ProviderUiContext *second = picker();
    require(first && second && second->component().toString().endsWith("ui/Selection.qml"),
        "the account's picker component is mounted with a context");
    engine.globalObject().setProperty("first", engine.newQObject(first));
    engine.globalObject().setProperty("second", engine.newQObject(second));
    engine.evaluate(QStringLiteral(R"JS(
        var firstCancelled = false, secondCompleted = false;
        first.request('delay', {milliseconds: 10000}).then(function() {}, function() { firstCancelled = true; });
        second.request('delay', {milliseconds: 10}).then(function() { secondCompleted = true; });
    )JS"));
    first->close();
    waitFor([&] {
        return engine.globalObject().property("firstCancelled").toBool()
            && engine.globalObject().property("secondCompleted").toBool();
    });
    require(!second->closed(), "dismissing one action does not close another action on the same source");
    require(QCoro::waitFor(registry.callSource(source, "bump")).value("calls").toInt() == 1,
        "action cancellation leaves the source context running");
    engine.evaluate(QStringLiteral(R"JS(
        var bulkRejected = false;
        second.request('candidates', {count: 2000}).then(function() {}, function() { bulkRejected = true; });
    )JS"));
    waitFor([&] { return engine.globalObject().property("bulkRejected").toBool(); });

    int batches = 0;
    bool modelThreadCorrect = true;
    QObject::connect(
        second->rows(), &QAbstractItemModel::rowsInserted, &app, [&](const QModelIndex&, int begin, int end) {
            ++batches;
            modelThreadCorrect = modelThreadCorrect && QThread::currentThread() == app.thread();
            require(end - begin + 1 <= 32, "large provider lists commit bounded native batches");
        });
    QQmlComponent component(
        &engine, QUrl::fromLocalFile(QStringLiteral(TEST_SOURCE_DIR "/tests/providers/fixtures/Selection.qml")));
    if (!component.isReady())
        std::cerr << component.errorString().toStdString();
    require(component.isReady(), "provider-owned selection component loads");
    std::unique_ptr<QObject> page(component.createWithInitialProperties({ { "action", QVariant::fromValue(second) } }));
    require(bool(page), "provider component receives a source-bound action context");
    waitFor([&] { return page->property("loaded").toBool() || page->property("failed").toBool(); });
    require(!page->property("failed").toBool() && second->rows()->rowCount() == 2000,
        "provider-owned virtualized UI receives every candidate through its native model");
    require(modelThreadCorrect && batches > 1, "all incremental model mutations run on the GUI thread");
    const auto record = second->rows()->data(second->rows()->index(1999), Qt::UserRole + 1).toMap();
    require(record.value("variantId") == "file-1999", "last candidate keeps its exact version identity");
    for (int i = 0; i < 100; ++i)
        second->rows()->data(second->rows()->index(i), Qt::UserRole + 1);
    require(QCoro::waitFor(registry.callSource(source, "state")).value("calls").toInt() == 1,
        "native role reads never call provider backend operations");
    const QPersistentModelIndex selectedIndex(second->rows()->index(1999));
    engine.evaluate(QStringLiteral(R"JS(
        var appended = false;
        second.requestList('candidates', {count: 2}, true).then(function() { appended = true; });
    )JS"));
    waitFor([&] { return engine.globalObject().property("appended").toBool(); });
    require(second->rows()->rowCount() == 2002 && selectedIndex.isValid()
            && selectedIndex.data(Qt::UserRole + 1).toMap().value("variantId") == "file-1999",
        "pagination preserves native selection identity instead of resetting the model");
    int finished = 0;
    QVariantMap selected;
    bool cancelled = true;
    QObject::connect(second, &ProviderUiContext::finished, &app, [&](const QVariantMap& value, bool isCancelled) {
        ++finished;
        selected = value;
        cancelled = isCancelled;
    });
    require(QMetaObject::invokeMethod(page.get(), "choose", Q_ARG(QVariant, QVariant("file-1999"))),
        "provider QML can return its selected variant");
    require(!cancelled && selected.value("variantId") == "file-1999", "the picker's choice is returned as given");
    page.reset();
    require(finished == 1, "completion and destruction settle the action exactly once");

    ProviderUiContext *dismissed = picker();
    QQmlComponent dismissComponent(&engine);
    dismissComponent.setData(
        "import QtQml\nQtObject { required property var action; Component.onDestruction: action.close() }", QUrl());
    std::unique_ptr<QObject> dismissedPage(
        dismissComponent.createWithInitialProperties({ { "action", QVariant::fromValue(dismissed) } }));
    require(bool(dismissedPage), "dismissal fixture creates");
    bool dismissedCancelled = false;
    QObject::connect(dismissed, &ProviderUiContext::finished, &app,
        [&](const QVariantMap&, bool value) { dismissedCancelled = value; });
    dismissedPage.reset();
    require(dismissedCancelled, "dismissing mounted provider UI returns cancellation");

    ProviderUiContext *destroyed = picker();
    engine.globalObject().setProperty("destroyedAction", engine.newQObject(destroyed));
    engine.evaluate(QStringLiteral(R"JS(
        var destroyedCancelled = false;
        destroyedAction.request('delay', {milliseconds: 10000}).then(function() {},
            function() { destroyedCancelled = true; });
    )JS"));
    delete destroyed;
    waitFor([&] { return engine.globalObject().property("destroyedCancelled").toBool(); });

    // pick(): the shell is asked to mount the picker and the choice comes back.
    QVariantMap answer { { "name", "Weekend" } };
    QObject::connect(&registry, &ProviderRegistry::componentRequested, &app, [&answer](QObject *context) {
        auto *screen = qobject_cast<ProviderUiContext *>(context);
        require(screen && screen->arguments().value("kind") == "name", "the picker gets what the provider asked for");
        if (answer.isEmpty())
            screen->close();
        else
            screen->complete(answer);
    });
    require(QCoro::waitFor(registry.pick(source, { { "kind", "name" } })) == answer, "a completed pick returns");
    answer.clear();
    require(QCoro::waitFor(registry.pick(source, { { "kind", "name" } })).isEmpty(), "a dismissed pick is empty");

    // Removing the account closes whatever screens it had open.
    ProviderUiContext *orphan = picker();
    registry.setAccountEnabled(source, false);
    require(orphan->closed(), "stopping a source closes its screens");
    return 0;
}
