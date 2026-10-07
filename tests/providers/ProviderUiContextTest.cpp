#include "provider/ProviderUiContext.h"
#include "ProviderFixture.h"
#include "TestMain.h"
#include "cache/DatabaseManager.h"
#include "diagnostics/Diagnostics.h"
#include "platform/CredentialStore.h"
#include "provider/Provider.h"
#include "provider/ProviderRegistry.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QPersistentModelIndex>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTcpServer>
#include <QTcpSocket>
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

SPOOL_TEST_MAIN("provider-ui-context")
{
#if !defined(Q_OS_ANDROID) && !defined(SPOOL_APPLE_MOBILE)
    qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    QGuiApplication app(argc, argv);
    using namespace Spool;
    QTemporaryDir directory;
    require(directory.isValid(), "isolated UI source database created");
    qputenv("SPOOL_CREDENTIAL_STORE_DIR", directory.filePath("credentials").toUtf8());
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
    waitFor([&] { return registry.sourceRunning(source); });
    QQmlEngine engine;
    auto *privateLogin = qobject_cast<ProviderUiContext *>(registry.beginSetup("fixture.test", source));
    require(privateLogin && !privateLogin->arguments().contains("setupAccount")
            && privateLogin->arguments().value("setupContext").toMap().value("accountId") == source,
        "login receives only approved nonsecret setup identity, never the retained account configuration");
    QTcpServer loginServer;
    require(loginServer.listen(QHostAddress::LocalHost), "login origin listener opens");
    int originHits = 0;
    QObject::connect(&loginServer, &QTcpServer::newConnection, &app, [&] {
        while (auto *socket = loginServer.nextPendingConnection()) {
            ++originHits;
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                socket->readAll();
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
                socket->disconnectFromHost();
            });
        }
    });
    const QString loginOrigin = QStringLiteral("http://127.0.0.1:%1").arg(loginServer.serverPort());
    bool originDenied = false;
    try {
        QCoro::waitFor(registry.callSource(privateLogin->sourceId(), "denied", { { "url", loginOrigin } }));
    } catch (const std::exception&) {
        originDenied = true;
    }
    require(originDenied && originHits == 0, "an unapproved login origin receives no native HTTP request");
    require(QCoro::waitFor(registry.callSource(privateLogin->sourceId(), "bump")).value("calls").toInt() == 1,
        "the private login closure has state before server approval");
    QCoro::waitFor(registry.allowSetupOrigin(privateLogin->sourceId(), QUrl(loginOrigin)));
    require(QCoro::waitFor(registry.callSource(privateLogin->sourceId(), "state")).value("calls").toInt() == 1,
        "approving a server preserves private login state instead of recreating the source");
    QCoro::waitFor(registry.callSource(privateLogin->sourceId(), "denied", { { "url", loginOrigin } }));
    require(originHits == 1, "the login server becomes reachable only after native origin approval");
    QString privateAccount;
    QVariantMap completedLogin;
    QStringList forwardedEvents;
    QVariantList forwardedPayloads;
    QObject::connect(&registry, &ProviderRegistry::accountAdded, &app, [&](const QString& id) { privateAccount = id; });
    QObject::connect(privateLogin, &ProviderUiContext::finished, &app, [&](const QVariantMap& value, bool cancelled) {
        require(!cancelled, "private login commits through the UI context");
        completedLogin = value;
    });
    QObject::connect(&registry, &ProviderRegistry::sourceStarted, &app, [&](Provider *provider) {
        QObject::connect(provider, &Provider::sourceEvent, &app, [&](const QString& type, const QVariantMap& value) {
            forwardedEvents.append(type);
            forwardedPayloads.append(value);
        });
    });
    engine.globalObject().setProperty("privateLogin", engine.newQObject(privateLogin));
    engine.evaluate(QStringLiteral(R"JS(
        var privateLoginDone = false, publicLoginResult = '';
        privateLogin.request('setupPrivate', {account: 'ui-linked', group: 'other-server', label: 'Private'})
            .then(function(result) {
                publicLoginResult = JSON.stringify(result);
                privateLogin.complete(result);
                privateLoginDone = true;
            });
    )JS"));
    waitFor([&] {
        return engine.globalObject().property("privateLoginDone").toBool() && !privateAccount.isEmpty()
            && registry.sourceRunning(privateAccount);
    });
    waitFor([&] { return CredentialStore::load(privateAccount).contains("ui-private-token"); });
    require(!engine.globalObject().property("publicLoginResult").toString().contains("ui-private-token")
            && !completedLogin.contains("configuration")
            && !QJsonDocument::fromVariant(registry.accounts()).toJson().contains("ui-private-token")
            && !Diagnostics::supportReportPreview().contains("ui-private-token"),
        "draft credentials persist without entering QML promises, completion, public account rows or support reports");
    QCoro::waitFor(registry.callSource(privateAccount, "setupPrivate",
        { { "account", "ui-linked" }, { "group", "other-server" }, { "label", "Private" },
            { "configuration", QVariantMap { { "token", "rotated-private-token" } } } }));
    waitFor([&] { return CredentialStore::load(privateAccount).contains("rotated-private-token"); });
    require(!forwardedEvents.contains("configuration")
            && !QJsonDocument::fromVariant(forwardedPayloads).toJson().contains("rotated-private-token"),
        "live credential updates stay private instead of reaching forwarded account events");
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
    engine.evaluate(QStringLiteral(R"JS(
        var activationError = '', activationListError = '', capabilityError = '';
        second.request('activate').then(function() {}, function(code) { activationError = code; });
        second.requestList('activate').then(function() {}, function(code) { activationListError = code; });
        second.request('suggestions').then(function() {}, function(code) { capabilityError = code; });
    )JS"));
    waitFor([&] {
        return !engine.globalObject().property("activationError").toString().isEmpty()
            && !engine.globalObject().property("activationListError").toString().isEmpty()
            && !engine.globalObject().property("capabilityError").toString().isEmpty();
    });
    require(engine.globalObject().property("activationError").toString() == "action_unavailable"
            && engine.globalObject().property("activationListError").toString() == "action_unavailable"
            && engine.globalObject().property("capabilityError").toString() == "unsupported_capability",
        "provider QML cannot invoke private activation or unoffered optional operations");
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

    auto discoveryPackage = ProviderFixture::package("fixture.test", "2.0.0");
    auto manifest = QJsonDocument::fromJson(discoveryPackage.files.value("manifest.json")).object();
    manifest.insert("capabilities", QJsonArray { "lanProbe" });
    discoveryPackage.files["manifest.json"] = QJsonDocument(manifest).toJson();
    discoveryPackage.manifest = *ProviderManifest::parse(discoveryPackage.files.value("manifest.json"));
    discoveryPackage.files["logic/provider.mjs"] = R"JS(
export function createSource() {
    return {
        discoverMore(args, host) { return host.delay(10000).then(function() { return {}; }); },
        delay(args, host) { return host.delay(20).then(function() { return {}; }); }
    };
}
)JS";
    QCoro::waitFor(registry.install(std::move(discoveryPackage)));
    auto *login = qobject_cast<ProviderUiContext *>(registry.beginSetup("fixture.test"));
    engine.globalObject().setProperty("login", engine.newQObject(login));
    engine.evaluate(QStringLiteral(R"JS(
        var consentError = '';
        login.allowLanDiscovery().then(function() {}, function(code) { consentError = code; });
    )JS"));
    waitFor([&] { return !registry.networkConsent().isEmpty(); });
    const QString dismissedConsentId = registry.networkConsent().value("id").toString();
    login->cancelLanDiscovery();
    waitFor([&] { return !engine.globalObject().property("consentError").toString().isEmpty(); });
    require(registry.networkConsent().isEmpty() && !login->closed(),
        "cancel local search dismisses its host consent without closing login");
    registry.resolveNetworkConsent(dismissedConsentId, true);
    engine.evaluate(QStringLiteral(R"JS(
        var consentReady = false;
        login.allowLanDiscovery().then(function() { consentReady = true; });
    )JS"));
    waitFor([&] { return !registry.networkConsent().isEmpty(); });
    registry.resolveNetworkConsent(registry.networkConsent().value("id").toString(), true);
    waitFor([&] { return engine.globalObject().property("consentReady").toBool(); });
    engine.evaluate(QStringLiteral(R"JS(
        var discoveryCancelled = false, unrelatedCompleted = false;
        login.request('discoverMore').then(function() {}, function() { discoveryCancelled = true; });
        login.request('delay').then(function() { unrelatedCompleted = true; });
    )JS"));
    login->cancelLanDiscovery();
    waitFor([&] {
        return engine.globalObject().property("discoveryCancelled").toBool()
            && engine.globalObject().property("unrelatedCompleted").toBool();
    });
    require(!login->closed(), "cancelling discovery leaves unrelated manual/UDP request scopes usable");
    engine.evaluate(QStringLiteral(R"JS(
        var closedConsentRejected = false;
        login.allowLanDiscovery().then(function() {}, function() { closedConsentRejected = true; });
    )JS"));
    waitFor([&] { return !registry.networkConsent().isEmpty(); });
    login->close();
    waitFor([&] { return engine.globalObject().property("closedConsentRejected").toBool(); });
    require(registry.networkConsent().isEmpty(), "closing login dismisses pending host consent");
    return 0;
}
