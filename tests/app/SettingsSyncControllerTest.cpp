#include "app/SettingsSyncController.h"
#include "../providers/ProviderFixture.h"
#include "TestMain.h"
#include "app/SettingsController.h"
#include "app/SettingsSchema.h"
#include "app/SettingsSyncDocument.h"
#include "cache/DatabaseManager.h"
#include "provider/ProviderRegistry.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLocale>
#include <QTemporaryDir>
#include <QThread>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>

using namespace Spool;
namespace Doc = Spool::SettingsSyncDocument;
namespace {
void require(bool value, const char *message)
{
    if (!value) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
void waitUntil(const std::function<bool()>& condition, const char *message, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(condition(), message);
}
QVariantMap entry(QVariant value, const QString& clock = QStringLiteral("100"), QChar nonce = QLatin1Char('a'))
{
    return { { "value", value }, { "clock", clock }, { "nonce", QString(32, nonce) } };
}
QVariantMap document(QVariantMap entries)
{
    return { { "format", 1 }, { "entries", entries } };
}
QVariantMap records(const QVariantMap& stats)
{
    return stats.value("document").toMap().value("entries").toMap();
}
QVariant remoteValue(const QVariantMap& stats, const QString& key)
{
    return records(stats).value(key).toMap().value("value");
}

struct Server {
    QTemporaryDir directory;
    DatabaseManager database;
    std::unique_ptr<ProviderRegistry> registry;
    Server()
    {
        require(directory.isValid(), "fixture temporary directory");
        qputenv("SPOOL_CREDENTIAL_STORE_DIR", directory.filePath("credentials").toUtf8());
        require(database.initialize(directory.filePath("registry.sqlite")), "registry database");
        auto package = ProviderFixture::package(QStringLiteral("fixture.settings"));
        auto manifest = QJsonDocument::fromJson(package.files.value("manifest.json")).object();
        manifest.insert(
            "extensions", QJsonObject { { "spool.settings-storage", 1 }, { "spool.playback-preferences", 1 } });
        package.files["manifest.json"] = QJsonDocument(manifest).toJson();
        package.manifest = *ProviderManifest::parse(package.files.value("manifest.json"));
        QFile script(QStringLiteral(TEST_SOURCE_DIR "/tests/providers/fixtures/settings-sync.mjs"));
        require(script.open(QIODevice::ReadOnly), "stateful settings fixture opens");
        package.files["logic/provider.mjs"] = script.readAll();
        const auto installs = directory.filePath("providers");
        require(ProviderPackage::install(package, installs).has_value(), "fixture installs");
        registry = std::make_unique<ProviderRegistry>(&database);
        registry->setInstallDirectory(installs);
        registry->loadModules();
        QCoro::waitFor(registry->restore());
    }
    QString add(const QString& name, QVariantMap config = {}, bool wait = true)
    {
        const auto id = registry->finishSetup({},
            { { "module", "fixture.settings" }, { "account", name }, { "label", name }, { "configuration", config } });
        registry->useAccount(id);
        if (wait)
            waitUntil([&] { return registry->sourceRunning(id); }, "source becomes active", 30000);
        return id;
    }
    QVariantMap call(const QString& id, const QString& operation, QVariantMap args = {})
    {
        return QCoro::waitFor(registry->callSource(id, operation, args));
    }
    QVariantMap stats(const QString& id)
    {
        return call(id, "stats");
    }
    void configure(const QString& id, QVariantMap args)
    {
        call(id, "configure", args);
    }
};
struct Replica {
    DatabaseManager database;
    std::unique_ptr<SettingsController> settings;
    std::unique_ptr<SettingsSyncController> sync;
    int cycles = 0;
    Replica(Server& server, const QString& name)
    {
        const auto replicaDirectory = server.directory.filePath(name);
        require(QDir().mkpath(replicaDirectory), "replica directory");
        require(database.initialize(QDir(replicaDirectory).filePath("cache.sqlite")), "replica database");
        settings = std::make_unique<SettingsController>(&database, nullptr, nullptr);
        sync = std::make_unique<SettingsSyncController>(settings.get(), &database, server.registry.get());
        settings->attachSync(sync.get());
        QObject::connect(sync.get(), &SettingsSyncController::cycleFinished, [&] { ++cycles; });
        QCoro::waitFor(settings->loadLocalAsync());
    }
    void load()
    {
        QCoro::waitFor(sync->loadLocalAsync());
    }
    void start()
    {
        load();
        waitUntil([&] { return cycles > 0 && !sync->busy(); }, "bootstrap completes");
    }
    void cycle()
    {
        const int before = cycles;
        sync->retry();
        waitUntil([&] { return cycles > before && !sync->busy(); }, "explicit sync attempt completes");
    }
    void edit(const QString& key, QVariant value)
    {
        QCoro::waitFor(settings->applyValues({ { key, value } }, ChangeOrigin::User));
    }
    QVariantMap state(const QString& key) const
    {
        return sync->states().value(key).toMap();
    }
    QVariantMap ledger()
    {
        return QJsonDocument::fromJson(
            QCoro::waitFor(database.loadSettingAsync("settingsSync/state/" + sync->accountId())).toUtf8())
            .object()
            .toVariantMap();
    }
};

void bootstrapAndChannels()
{
    Server server;
    const auto account = server.add("one",
        { { "document",
              document({ { "theme/reducedMotion", entry(true) },
                  { "future/newOption", entry(QVariantMap { { "nested", 7 } }) },
                  { "appearance/uiScalePercent", entry(175) },
                  { "playback/mpvConfigDirectory", entry("/private/path") }, { "credentials/token", entry("secret") },
                  { "client/deviceId", entry("installation-id") },
                  { "tls/trusted/example/certificatePem", entry("private trust") },
                  { "uiSession/snapshot", entry(QVariantMap { { "itemId", "private-item" } }) } }) },
            { "native",
                QVariantMap { { "audioLanguage", "fr" }, { "audioMode", "Smart" }, { "subtitleLanguage", "eng" },
                    { "subtitleMode", "OnlyForced" } } } });
    Replica replica(server, "local");
    replica.start();
    require(replica.sync->enabled() && replica.sync->accountId() == account,
        "sync is default-on and chooses the eligible account");
    require(replica.settings->value("theme/reducedMotion").toBool(), "existing remote portable value wins bootstrap");
    require(QLocale::codeToLanguage(replica.settings->value("audio/language").toString()) == QLocale::French,
        "native language wins bootstrap");
    require(replica.settings->uiScalePercent() == 100, "remote scale cannot cross the local-only boundary");
    auto stats = server.stats(account);
    require(stats.value("preferenceWrites").toList().isEmpty(), "native bootstrap does not echo a remote read");
    require(!records(stats).contains("audio/language") && !records(stats).contains("appearance/uiScalePercent")
            && !records(stats).contains("playback/mpvConfigDirectory") && !records(stats).contains("credentials/token"),
        "native fields and known Never/sensitive keys are absent from outgoing documents");
    require(!records(stats).contains("client/deviceId")
            && !records(stats).contains("tls/trusted/example/certificatePem")
            && !records(stats).contains("uiSession/snapshot"),
        "canonical identity, certificate trust and session-recovery stores never survive an outgoing merge");
    require(records(stats).value("future/newOption").toMap().value("value").toMap().value("nested").toInt() == 7,
        "unknown bounded future data survives initialization writes");
    require(replica.state("audio/language").value("backend") == "native"
            && replica.state("theme/reducedMotion").value("backend") == "spool",
        "writable native mapping takes precedence");
    replica.edit("audio/language", "deu");
    replica.edit("theme/reducedMotion", false);
    replica.cycle();
    stats = server.stats(account);
    require(QLocale::codeToLanguage(stats.value("native").toMap().value("audioLanguage").toString()) == QLocale::German
            && !remoteValue(stats, "theme/reducedMotion").toBool(),
        "one local native edit and one document edit reach their authoritative channels");
    require(replica.state("audio/language").value("status") == "synced"
            && replica.state("theme/reducedMotion").value("status") == "synced",
        "confirmed readbacks expose synchronized state");

    server.configure(account, { { "writable", QVariantList { "audioMode", "subtitleLanguage", "subtitleMode" } } });
    replica.cycle();
    require(replica.state("audio/language").value("backend") == "spool",
        "read-only native field falls back only to real application storage");
    replica.edit("audio/language", "ita");
    replica.cycle();
    stats = server.stats(account);
    require(QLocale::codeToLanguage(remoteValue(stats, "audio/language").toString()) == QLocale::Italian
            && QLocale::codeToLanguage(stats.value("native").toMap().value("audioLanguage").toString())
                == QLocale::German,
        "fallback does not also write a read-only native preference");
}

void provisionalAndReadbackRaces()
{
    Server server;
    const auto account
        = server.add("races", { { "document", document({ { "theme/reducedMotion", entry(true, "100") } }) } });
    server.configure(account, { { "options", QVariantMap { { "readDelay", 180 } } } });
    Replica replica(server, "races");
    replica.load();
    waitUntil([&] { return server.stats(account).value("storageReads").toInt() > 0; }, "bootstrap read starts");
    replica.edit("theme/reducedMotion", true);
    replica.edit("theme/reducedMotion", false);
    require(replica.ledger()
                .value("keys")
                .toMap()
                .value("theme/reducedMotion")
                .toMap()
                .value("intent")
                .toMap()
                .value("provisional")
                .toBool(),
        "in-flight bootstrap edits are durably provisional, not prematurely clocked");
    waitUntil([&] { return replica.cycles > 0 && !replica.sync->busy(); }, "provisional bootstrap finishes");
    replica.cycle();
    const auto saved = records(server.stats(account)).value("theme/reducedMotion").toMap();
    require(!saved.value("value").toBool() && Doc::compareClocks(saved.value("clock").toString(), "100") > 0,
        "a provisional edit is stamped above the remote clock observed after it was committed");

    server.configure(account, { { "options", QVariantMap { { "readDelay", 0 }, { "nativeWriteDelay", 180 } } } });
    replica.edit("audio/language", "fra");
    const int nativeBefore = server.stats(account).value("preferenceWrites").toList().size();
    replica.sync->retry();
    waitUntil([&] { return server.stats(account).value("preferenceWrites").toList().size() > nativeBefore; },
        "native write begins");
    replica.edit("audio/language", "deu");
    waitUntil([&] { return !replica.sync->busy(); }, "older native readback settles");
    require(replica.ledger().value("keys").toMap().value("audio/language").toMap().contains("intent"),
        "an old native write cannot acknowledge a later generation");
    replica.cycle();
    require(QLocale::codeToLanguage(server.stats(account).value("native").toMap().value("audioLanguage").toString())
            == QLocale::German,
        "later native edit survives the earlier readback");

    server.configure(account, { { "options", QVariantMap { { "nativeWriteDelay", 0 }, { "writeDelay", 180 } } } });
    replica.edit("theme/reducedMotion", true);
    const int before = server.stats(account).value("writes").toList().size();
    replica.sync->retry();
    waitUntil([&] { return server.stats(account).value("writes").toList().size() > before; }, "document write begins");
    replica.edit("theme/reducedMotion", false);
    waitUntil([&] { return !replica.sync->busy(); }, "older document cycle settles");
    replica.cycle();
    require(!remoteValue(server.stats(account), "theme/reducedMotion").toBool(),
        "document readback cannot erase a newer generation");
}

void weakConvergenceAndCas()
{
    Server server;
    const auto account = server.add("weak");
    Replica first(server, "first");
    first.start();
    Replica second(server, "second");
    second.start();
    first.edit("theme/reducedMotion", true);
    first.cycle();
    const auto acknowledged = records(server.stats(account)).value("theme/reducedMotion");
    auto raced = records(server.stats(account));
    raced.remove("theme/reducedMotion");
    raced.insert("theme/railLabels", entry("Always", "10000", QLatin1Char('b')));
    server.configure(account, { { "document", document(raced) } });
    second.cycle();
    first.cycle();
    second.cycle();
    const auto converged = records(server.stats(account));
    require(converged.value("theme/reducedMotion") == acknowledged
            && converged.value("theme/railLabels") == raced.value("theme/railLabels"),
        "retained acknowledged maxima repair weak overwrites while preserving another device's different-key edit");
    require(
        first.settings->value("theme/railLabels") == "Always" && second.settings->value("theme/reducedMotion").toBool(),
        "two independent local replicas converge after reconnecting");

    server.configure(
        account, { { "options", QVariantMap { { "overwriteCount", 2 }, { "concurrentDocument", document({}) } } } });
    first.edit("theme/reducedMotion", false);
    const int before = server.stats(account).value("writes").toList().size();
    first.cycle();
    require(server.stats(account).value("writes").toList().size() == before + 2
            && first.state("theme/reducedMotion").value("status") == "error",
        "weak writes stop after two raced readbacks");
    require(first.ledger().value("replica").toMap().value("entries").toMap().contains("theme/reducedMotion"),
        "missing readback never erases the durable per-key maximum");
    first.cycle();
    require(!remoteValue(server.stats(account), "theme/reducedMotion").toBool(),
        "later explicit refresh repairs without requiring a new edit");

    const auto cas = server.add("cas", { { "cas", true } });
    first.sync->setAccountId(cas);
    require(first.sync->accountId() == account && first.sync->accountChangePending(),
        "source selection requires confirmation");
    first.sync->confirmAccountChange(true);
    first.cycle();
    const auto competing = document({ { "future/concurrent", entry("retained", "500") } });
    server.configure(
        cas, { { "options", QVariantMap { { "conflictOnce", true }, { "concurrentDocument", competing } } } });
    first.edit("theme/reducedMotion", true);
    first.cycle();
    require(remoteValue(server.stats(cas), "future/concurrent") == "retained"
            && remoteValue(server.stats(cas), "theme/reducedMotion").toBool(),
        "a real CAS conflict rereads and merges the competing revision");
    const auto casWrites = server.stats(cas).value("writes").toList();
    require(casWrites.last().toMap().contains("expectedRevision"), "CAS uses the actual backend revision");
}

void optOutSourceAndRestart()
{
    Server server;
    const auto a = server.add("A");
    const auto b = server.add("B", { { "document", document({ { "theme/reducedMotion", entry(true) } }) } });
    auto replica = std::make_unique<Replica>(server, "persisted");
    replica->start();
    replica->sync->setSettingEnabled("theme/reducedMotion", false);
    replica->edit("theme/reducedMotion", true);
    replica->cycle();
    require(!remoteValue(server.stats(a), "theme/reducedMotion").toBool(),
        "per-key opt-out keeps a local edit off the server");
    auto remote = records(server.stats(a));
    remote.insert("theme/reducedMotion", entry(false, "10000"));
    server.configure(a, { { "document", document(remote) } });
    replica->edit("theme/railLabels", "Always");
    replica->cycle();
    require(records(server.stats(a)).value("theme/reducedMotion") == remote.value("theme/reducedMotion"),
        "opted-out writes carry latest remote records, never a retained local winner");
    replica->sync->setSettingEnabled("theme/reducedMotion", true);
    replica->cycle();
    require(!replica->settings->value("theme/reducedMotion").toBool(), "re-enable is remote-first");

    server.configure(a, { { "options", QVariantMap { { "offline", true } } } });
    replica->edit("theme/reducedMotion", true);
    replica->cycle();
    require(replica->state("theme/reducedMotion").value("status") == "offline",
        "an unreachable backend is distinguished from invalid data while its source remains active");
    require(replica->ledger().value("keys").toMap().value("theme/reducedMotion").toMap().contains("intent"),
        "offline local intent is durable");
    replica.reset();
    replica = std::make_unique<Replica>(server, "persisted");
    replica->start();
    server.configure(a, { { "options", QVariantMap { { "offline", false } } } });
    replica->cycle();
    require(remoteValue(server.stats(a), "theme/reducedMotion").toBool(),
        "restart/reconnect to same source retains dirty intent");

    server.configure(a, { { "options", QVariantMap { { "offline", true } } } });
    replica->edit("theme/reducedMotion", false);
    replica->sync->setAccountId(b);
    replica->sync->confirmAccountChange(false);
    require(replica->sync->accountId() == a, "cancelled source switch retains authority");
    replica->sync->setAccountId(b);
    replica->sync->confirmAccountChange(true);
    replica->cycle();
    require(
        replica->settings->value("theme/reducedMotion").toBool(), "destination source bootstraps its own remote state");
    server.configure(a, { { "options", QVariantMap { { "offline", false } } } });
    replica->sync->setAccountId(a);
    replica->sync->confirmAccountChange(true);
    replica->cycle();
    require(
        replica->settings->value("theme/reducedMotion").toBool(), "A to B to A discards A's departing unsent intent");
    replica->sync->setEnabled(false);
    replica->edit("theme/reducedMotion", false);
    const int writes = server.stats(a).value("writes").toList().size();
    replica.reset();
    replica = std::make_unique<Replica>(server, "persisted");
    replica->load();
    require(!replica->sync->enabled() && !replica->settings->value("theme/reducedMotion").toBool(),
        "off plus local edit plus restart never replays the remote replica");
    require(server.stats(a).value("writes").toList().size() == writes, "disabled restart performs no network writes");
    replica->sync->setEnabled(true);
    replica->cycle();
    server.registry->removeAccount(a);
    waitUntil([&] { return !replica->sync->busy(); }, "removed source pauses");
    require(replica->sync->accountId() == a && !replica->sync->summary().isEmpty(),
        "removing the selected source retains an explicit selection prompt instead of importing B");
}

void editDeferralMalformedAndPolicy()
{
    Server server;
    const auto account = server.add("policy");
    Replica replica(server, "policy");
    replica.start();
    replica.sync->beginEdit("theme/reducedMotion");
    auto remote = records(server.stats(account));
    remote.insert("theme/reducedMotion", entry(true, "10000"));
    server.configure(account, { { "document", document(remote) } });
    replica.cycle();
    require(
        !replica.settings->value("theme/reducedMotion").toBool(), "incoming value is deferred while the row is edited");
    replica.sync->endEdit("theme/reducedMotion", false);
    waitUntil([&] { return replica.settings->value("theme/reducedMotion").toBool(); },
        "cancel/no-change applies queued remote value");
    replica.sync->beginEdit("theme/reducedMotion");
    replica.edit("theme/reducedMotion", false);
    const auto writeCount = server.stats(account).value("writes").toList().size();
    replica.cycle();
    require(server.stats(account).value("writes").toList().size() == writeCount,
        "editing pauses outgoing intent for that row");
    replica.sync->endEdit("theme/reducedMotion", true);
    replica.cycle();
    require(!remoteValue(server.stats(account), "theme/reducedMotion").toBool(),
        "completed user edit wins over queued remote application");

    remote = records(server.stats(account));
    remote.insert("theme/railLabels", entry("Future Unsupported Choice", "20000"));
    remote.insert("subtitles/font", entry("system:RemoteDeviceOnlyFont", "20001"));
    server.configure(account, { { "document", document(remote) } });
    replica.cycle();
    require(replica.settings->value("theme/railLabels") != "Future Unsupported Choice"
            && !replica.settings->value("subtitles/font").toString().startsWith("system:"),
        "unsupported enum and unopted system font remain unapplied rather than clamped");
    replica.edit("theme/reducedMotion", true);
    replica.cycle();
    require(records(server.stats(account)).value("theme/railLabels") == remote.value("theme/railLabels")
            && records(server.stats(account)).value("subtitles/font") == remote.value("subtitles/font"),
        "unsupported values and remote device font survive unrelated writes intact");
    replica.edit("subtitles/font", "system:LocalDeviceFont");
    replica.cycle();
    require(remoteValue(server.stats(account), "subtitles/font") == "system:RemoteDeviceOnlyFont",
        "a local system font cancels unsent font intent without explicit opt-in");
    remote = records(server.stats(account));
    remote.insert("subtitles/font", entry("interface", "30000"));
    server.configure(account, { { "document", document(remote) } });
    replica.edit("subtitles/font", "");
    replica.cycle();
    require(remoteValue(server.stats(account), "subtitles/font") == "interface"
            && replica.settings->value("subtitles/font") == "interface",
        "returning to bundled font re-enables remote-first rather than stamping the local fallback");

    for (const auto& invalid : { QVariantMap { { "format", 2 }, { "entries", QVariantMap {} } },
             document({ { "theme/reducedMotion",
                 QVariantMap { { "value", false }, { "clock", "bad" }, { "nonce", "invalid" } } } }),
             document({ { "future/oversized", entry(QString(66000, QLatin1Char('x'))) } }) }) {
        server.configure(account, { { "document", invalid } });
        const auto before = server.stats(account).value("writes").toList().size();
        replica.edit("theme/reducedMotion", !replica.settings->value("theme/reducedMotion").toBool());
        const auto local = replica.settings->value("theme/reducedMotion");
        replica.cycle();
        require(server.stats(account).value("writes").toList().size() == before
                && replica.settings->value("theme/reducedMotion") == local
                && replica.state("theme/reducedMotion").value("status") == "error",
            "malformed/unknown-format document stops writes and preserves local values");
    }
}

void interruptedBootstrapAndSourceCancellation()
{
    Server server;
    const auto a = server.add("pending A", { { "document", document({ { "theme/reducedMotion", entry(false) } }) } });
    const auto b
        = server.add("pending B", { { "document", document({ { "theme/reducedMotion", entry(true, "500") } }) } });
    server.configure(a, { { "options", QVariantMap { { "readDelay", 600 } } } });
    auto replica = std::make_unique<Replica>(server, "interrupted");
    replica->load();
    waitUntil([&] { return server.stats(a).value("storageReads").toInt() > 0; }, "initial read is pending");
    replica->edit("theme/reducedMotion", true);
    replica.reset();
    server.configure(a, { { "options", QVariantMap { { "readDelay", 0 } } } });
    replica = std::make_unique<Replica>(server, "interrupted");
    replica->start();
    const auto restored = records(server.stats(a)).value("theme/reducedMotion").toMap();
    require(restored.value("value").toBool() && Doc::compareClocks(restored.value("clock").toString(), "100") > 0,
        "restart during bootstrap retains provisional value and stamps after the first successful observation");

    auto remote = records(server.stats(a));
    remote.insert("theme/reducedMotion", entry(false, "10000"));
    server.configure(a, { { "document", document(remote) }, { "options", QVariantMap { { "readDelay", 300 } } } });
    const int reads = server.stats(a).value("storageReads").toInt();
    replica->sync->retry();
    waitUntil([&] { return server.stats(a).value("storageReads").toInt() > reads; }, "old source read starts");
    replica->sync->setAccountId(b);
    replica->sync->confirmAccountChange(true);
    waitUntil(
        [&] {
            return replica->sync->accountId() == b && !replica->sync->busy()
                && replica->state("theme/reducedMotion").value("status") == "synced";
        },
        "new selected source bootstraps");
    require(replica->settings->value("theme/reducedMotion").toBool(),
        "late old-source data cannot apply after a confirmed authority change");
    const auto before = server.stats(b).value("writes").toList().size();
    replica->sync->setForeground(false);
    replica->edit("theme/reducedMotion", false);
    replica->sync->retry();
    require(server.stats(b).value("writes").toList().size() == before,
        "suspended synchronization does not dispatch even an explicit pending edit");
    replica->sync->setForeground(true);
    replica->cycle();
    require(!remoteValue(server.stats(b), "theme/reducedMotion").toBool(),
        "foreground retry resumes the durable suspended intent");
}

void unsupportedPlatformAndSmallStore()
{
    Server server;
    const auto account = server.add("platform");
    Replica replica(server, "platform");
    replica.start();
    auto remote = records(server.stats(account));
    QString unsupported;
    QVariant unsupportedValue;
    for (const auto& spec : settingSpecs()) {
        if (spec.syncPolicy != SettingSyncPolicy::Never && !settingSupportedOnPlatform(spec)) {
            unsupported = QString::fromLatin1(spec.key);
            unsupportedValue = settingDefaultValue(spec);
            remote.insert(unsupported, entry(unsupportedValue, "10000"));
            break;
        }
    }
    require(!unsupported.isEmpty(), "schema provides a setting for a different platform");
    server.configure(account, { { "document", document(remote) } });
    replica.sync->setSettingEnabled(unsupported, true);
    replica.cycle();
    require(!replica.state(unsupported).value("eligible").toBool()
            && records(server.stats(account)).value(unsupported) == remote.value(unsupported),
        "unsupported platform values are preserved but cannot be opted into or applied");

    const auto small = server.add("bounded store", { { "maxBytes", 512 } });
    replica.sync->setAccountId(small);
    replica.sync->confirmAccountChange(true);
    replica.cycle();
    require(server.stats(small).value("writes").toList().isEmpty()
            && replica.state("theme/reducedMotion").value("status") == "error",
        "provider's smaller document budget rejects writes without discarding durable local intent");
}

void persistentSelectionOrder()
{
    Server server;
    const auto first = server.add("Z first connection", { { "describeDelay", 200 } }, false);
    const auto second = server.add("A second connection", {}, false);
    Replica replica(server, "ordered");
    replica.load();
    waitUntil([&] { return server.registry->sourceRunning(second); }, "later fast source starts");
    // Slow runners can finish the first describe while Replica is loading.
    // Either waiting for it or already selecting it is correct; the second
    // account must never become the authority just because it replied first.
    require(replica.sync->accountId().isEmpty() || replica.sync->accountId() == first,
        "earlier starting source is not overtaken by alphabetical or response order");
    waitUntil([&] { return replica.cycles > 0 && !replica.sync->busy(); }, "earlier source resolves");
    require(replica.sync->accountId() == first, "persistent first connected eligible source owns sync");
    server.registry->setAccountEnabled(first, false);
    require(replica.sync->accountId() == first, "disabling selected source never automatically switches authority");
}

void unavailableNativePreferencesUseSpoolStorage()
{
    Server server;
    const auto account = server.add("storage fallback");
    server.configure(account,
        { { "options", QVariantMap { { "preferencesUnavailable", true } } },
            { "document", document({ { "audio/language", entry("eng") } }) } });
    Replica replica(server, "fallback");
    replica.start();
    require(replica.state("audio/language").value("backend") == "spool"
            && replica.settings->value("audio/language") == "eng",
        "an absent native preference endpoint uses available application storage with remote-first authority");
    replica.edit("audio/trackMode", "Smart");
    replica.cycle();
    const auto state = server.stats(account);
    require(remoteValue(state, "audio/trackMode") == "Smart" && state.value("preferenceWrites").toList().isEmpty(),
        "fallback writes only the Spool channel, never the unavailable service preference writer");
}

void boundedFutureKeysSurviveRestart()
{
    Server server;
    QVariantMap future;
    for (int index = 0; index < 513; ++index)
        future.insert(QStringLiteral("future/key%1").arg(index), entry(index));
    const auto account = server.add("future fields", { { "document", document(future) } });
    {
        Replica replica(server, "future-replica");
        replica.start();
        require(replica.state("theme/reducedMotion").value("status") == "synced",
            "a bounded document with many unknown future fields remains writable");
    }
    Replica restarted(server, "future-replica");
    restarted.start();
    require(restarted.state("theme/reducedMotion").value("status") == "synced",
        "every accepted bounded replica can be restored after restart");
    const auto retained = records(server.stats(account));
    for (auto it = future.cbegin(); it != future.cend(); ++it)
        require(retained.value(it.key()) == it.value(), "unknown future records survive local restart unchanged");
}
}

SPOOL_TEST_MAIN("settings-sync-controller")
{
    QCoreApplication app(argc, argv);
    bootstrapAndChannels();
    provisionalAndReadbackRaces();
    weakConvergenceAndCas();
    optOutSourceAndRestart();
    editDeferralMalformedAndPolicy();
    persistentSelectionOrder();
    interruptedBootstrapAndSourceCancellation();
    unsupportedPlatformAndSmallStore();
    boundedFutureKeysSurviveRestart();
    unavailableNativePreferencesUseSpoolStorage();
    return EXIT_SUCCESS;
}
