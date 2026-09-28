#include "app/CollectionEditingController.h"
#include "../providers/ProviderFixture.h"
#include "TestMain.h"
#include "cache/DatabaseManager.h"
#include "provider/ProviderRegistry.h"
#include "provider/SourceHub.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdlib>
#include <functional>
#include <iostream>

using namespace Spool;
namespace {
void require(bool value, const char *message)
{
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void waitUntil(const std::function<bool()>& condition, const char *message, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(condition(), message);
}
QStringList entryIds(const CollectionEditingController& editor)
{
    QStringList result;
    for (const auto& value : editor.entries())
        result.append(value.toMap().value(QStringLiteral("entryId")).toString());
    return result;
}
}

SPOOL_TEST_MAIN("collection-editing")
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage");
    qputenv("SPOOL_CREDENTIAL_STORE_DIR", directory.filePath(QStringLiteral("credentials")).toUtf8());
    DatabaseManager database;
    require(database.initialize(directory.filePath(QStringLiteral("cache.sqlite"))), "database opens");
    auto package = ProviderFixture::package(QStringLiteral("fixture.collections"));
    auto manifest = QJsonDocument::fromJson(package.files.value(QStringLiteral("manifest.json"))).object();
    manifest.insert(QStringLiteral("extensions"),
        QJsonObject { { QStringLiteral("spool.collection-editing"), 1 }, { QStringLiteral("spool.item-actions"), 1 } });
    package.files[QStringLiteral("manifest.json")] = QJsonDocument(manifest).toJson();
    package.manifest = *ProviderManifest::parse(package.files.value(QStringLiteral("manifest.json")));
    QFile script(QStringLiteral(TEST_SOURCE_DIR "/tests/providers/fixtures/collections.mjs"));
    require(script.open(QIODevice::ReadOnly), "collection fixture opens");
    package.files[QStringLiteral("logic/provider.mjs")] = script.readAll();
    const auto installs = directory.filePath(QStringLiteral("providers"));
    require(ProviderPackage::install(package, installs).has_value(), "collection fixture installs");
    ProviderRegistry registry(&database);
    registry.setInstallDirectory(installs);
    registry.loadModules();
    SourceHub hub(&registry);
    QCoro::waitFor(registry.restore());
    const QString account = registry.finishSetup({},
        { { QStringLiteral("module"), QStringLiteral("fixture.collections") },
            { QStringLiteral("account"), QStringLiteral("one") },
            { QStringLiteral("label"), QStringLiteral("Collection account") },
            { QStringLiteral("configuration"), QVariantMap {} } });
    registry.useAccount(account);
    waitUntil([&] { return hub.source(account) != nullptr; }, "collection source starts", 30000);
    const QString container = hub.scoped(account, QStringLiteral("list"));
    CollectionEditingController editor(&hub);
    const auto setup = [&](const QString& mode = {}) {
        editor.close();
        QCoro::waitFor(hub.call(account, QStringLiteral("setup"), { { QStringLiteral("mode"), mode } }));
    };
    const auto open = [&] {
        editor.open(container, QStringLiteral("Playlist"));
        waitUntil([&] { return !editor.busy(); }, "collection loads");
    };
    const auto settled = [&] { waitUntil([&] { return !editor.busy(); }, "collection operation settles"); };
    const auto stats = [&] { return QCoro::waitFor(hub.call(account, QStringLiteral("stats"))); };

    setup();
    open();
    require(entryIds(editor) == QStringList({ "first", "second" }) && editor.hasMore(),
        "first page retains both occurrences of the same media");
    require(editor.entries()[0].toMap().value("itemId") == editor.entries()[1].toMap().value("itemId")
            && editor.entries()[0].toMap().value("itemId").toString() == hub.scoped(account, "same"),
        "media identities are scoped without changing opaque occurrence IDs");
    editor.selectEntry(QStringLiteral("second"));
    editor.removeEntry(QStringLiteral("second"));
    editor.removeEntry(QStringLiteral("first"));
    settled();
    require(entryIds(editor) == QStringList({ "first", "third" }) && editor.selectedEntryId() == QStringLiteral("third")
            && editor.selectedIndex() == 1,
        "removing the second duplicate retains the first and focuses its nearest surviving neighbor");
    require(
        stats().value("mutations").toList().size() == 1, "a second mutation while one is pending is not dispatched");

    setup();
    open();
    editor.moveEntry(QStringLiteral("second"), 1);
    settled();
    require(editor.problem().isEmpty() && entryIds(editor) == QStringList({ "first", "third", "second", "fourth" })
            && editor.selectedEntryId() == QStringLiteral("second") && editor.selectedIndex() == 2,
        "moving across a page boundary loads the neighbor and retains occurrence focus");
    const auto boundary = stats();
    const auto movement = boundary.value("mutations").toList().first().toMap();
    require(movement.value("index").toInt() == 2 && movement.value("afterEntryId").toString() == "third",
        "move uses the same post-removal destination for index and anchor");
    require(!boundary.value("reads").toList().contains(QStringLiteral("p:4"))
            && boundary.value("reads").toList().contains(QStringLiteral("gap")),
        "an advancing empty page is traversed but the whole container is not fetched for an anchor");
    editor.moveEntry(QStringLiteral("second"), -1);
    settled();
    const auto upward = stats().value("mutations").toList().last().toMap();
    require(entryIds(editor) == QStringList({ "first", "second", "third", "fourth" })
            && upward.value("index").toInt() == 1 && upward.value("afterEntryId").toString() == "first",
        "moving upward uses the preceding post-removal occurrence, not the displaced row");

    setup(QStringLiteral("after"));
    open();
    editor.moveEntry(QStringLiteral("second"), -1);
    settled();
    const auto firstMove = stats().value("mutations").toList().first().toMap();
    require(entryIds(editor) == QStringList({ "second", "first" }) && firstMove.value("index").toInt() == 0
            && firstMove.contains("afterEntryId") && firstMove.value("afterEntryId").isNull(),
        "after-style moves to the first position carry an explicit null anchor");

    setup(QStringLiteral("unordered"));
    open();
    require(editor.removable() && !editor.movable(), "unordered collections permit removal but not movement");
    editor.moveEntry(QStringLiteral("first"), 1);
    require(stats().value("mutations").toList().isEmpty(), "unordered move is refused before provider dispatch");
    setup(QStringLiteral("smart"));
    open();
    require(!editor.removable() && !editor.movable(), "smart collections have no mutation controls");
    editor.removeEntry(QStringLiteral("first"));
    require(stats().value("mutations").toList().isEmpty(), "smart collection removal does not reach the provider");

    setup(QStringLiteral("uncertain"));
    open();
    editor.removeEntry(QStringLiteral("second"));
    settled();
    require(entryIds(editor) == QStringList({ "first", "third" }) && !editor.problem().isEmpty()
            && stats().value("mutations").toList().size() == 1,
        "a committed mutation with a lost response is refetched, not rolled back or repeated");
    setup(QStringLiteral("denied"));
    open();
    editor.removeEntry(QStringLiteral("second"));
    settled();
    require(
        entryIds(editor) == QStringList({ "first", "second" }) && !editor.removable() && !editor.problem().isEmpty(),
        "permission failure reloads policy and preserves authoritative entries");

    for (const auto& mode :
        { QStringLiteral("missing"), QStringLiteral("repeat"), QStringLiteral("duplicate-entry") }) {
        setup(mode);
        open();
        require(!editor.problem().isEmpty() && editor.entries().isEmpty() && !editor.removable(),
            "invalid pagination or repeated occurrence identity cannot leave stale editable rows");
    }
    setup(QStringLiteral("forever"));
    open();
    require(!editor.problem().isEmpty() && stats().value("reads").toList().size() == 256,
        "requested-prefix collection is bounded even when every advancing page is empty");

    setup(QStringLiteral("slow"));
    editor.open(container, QStringLiteral("Slow"));
    editor.close();
    bool elapsed = false;
    QTimer::singleShot(150, [&] { elapsed = true; });
    waitUntil([&] { return elapsed; }, "late result window passes");
    require(editor.entries().isEmpty() && editor.containerId().isEmpty() && !editor.busy(),
        "closing an editor cancels its scope and prevents a late result from repopulating it");

    setup();
    const QString item = hub.scoped(account, QStringLiteral("same"));
    require(stats().value("actionReads").toInt() == 0, "catalogue use does not fetch item menus eagerly");
    int completed = -1;
    QVariantList actions;
    QString menuProblem;
    QObject::connect(
        &hub, &SourceHub::itemActionsReady, [&](int request, const QVariantList& result, const QString& problem) {
            completed = request;
            actions = result;
            menuProblem = problem;
        });
    const int menu = hub.requestItemActions(item, QStringLiteral("Movie"), container, QStringLiteral("second"));
    waitUntil([&] { return completed == menu; }, "negotiated item actions complete");
    require(menuProblem.isEmpty() && actions.size() == 2 && actions[0].toMap().value("id").toString() == "allowed"
            && !actions[1].toMap().value("enabled").toBool(),
        "negotiated account policy replaces the manifest action list and retains disabled reasons");
    bool denied = false;
    QObject::connect(&hub, &Provider::toastRequested, [&](const QString&) { denied = true; });
    hub.runItemAction(QStringLiteral("denied"), item, QStringLiteral("Movie"), container, QStringLiteral("second"));
    waitUntil([&] { return denied; }, "disabled action is rejected");
    require(stats().value("actionRuns").toInt() == 0, "disabled actions never invoke provider execution");
    hub.runItemAction(QStringLiteral("allowed"), item, QStringLiteral("Movie"), container, QStringLiteral("second"));
    waitUntil([&] { return stats().value("actionRuns").toInt() == 1; }, "allowed action executes");

    setup(QStringLiteral("slow"));
    const int stale = hub.requestItemActions(item, QStringLiteral("Movie"));
    hub.cancelItemActions();
    elapsed = false;
    QTimer::singleShot(150, [&] { elapsed = true; });
    waitUntil([&] { return elapsed; }, "cancelled menu window passes");
    require(completed != stale, "a closed menu cannot publish stale permission results");

    setup();
    open();
    registry.setAccountEnabled(account, false);
    require(editor.entries().isEmpty() && !editor.removable() && !editor.busy(),
        "stopping the owning account clears the editor even after its scoped-ID mapping is removed");
    registry.setAccountEnabled(account, true);
    waitUntil([&] { return hub.source(account) != nullptr; }, "collection source restarts");
    setup();
    open();
    QCoro::waitFor(hub.call(account, QStringLiteral("dropExtensions")));
    waitUntil([&] { return !hub.collectionEditingAvailable(container); }, "extension support is revoked");
    require(editor.entries().isEmpty() && !editor.removable() && !editor.movable(),
        "support loss clears visible entries and mutation controls");
    const int lost = hub.requestItemActions(item, QStringLiteral("Movie"));
    waitUntil([&] { return completed == lost; }, "unsupported menu resolves");
    require(actions.isEmpty(), "a new provider cannot fall back to unfiltered manifest actions after support loss");
    return 0;
}
