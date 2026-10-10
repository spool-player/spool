#include "app/RemoteTargetsController.h"
#include "../providers/ProviderFixture.h"
#include "TestMain.h"
#include "cache/DatabaseManager.h"
#include "provider/ProviderRegistry.h"
#include "provider/ProviderUiContext.h"
#include "provider/SourceHub.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <algorithm>
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
void waitUntil(const std::function<bool()>& condition, const char *message, int deadline = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(condition(), message);
}
void elapse(int milliseconds)
{
    bool elapsed = false;
    QTimer::singleShot(milliseconds, [&] { elapsed = true; });
    waitUntil([&] { return elapsed; }, "event window passes", milliseconds + 2000);
}
template <typename T> QString failure(QCoro::Task<T> task)
{
    try {
        QCoro::waitFor(std::move(task));
    } catch (const std::exception& error) {
        return QString::fromUtf8(error.what());
    }
    return {};
}
QStringList entries(const RemoteTargetsController& remote)
{
    QStringList result;
    for (const auto& row : remote.queue())
        result.append(row.toMap().value("entryId").toString());
    return result;
}
}

SPOOL_TEST_MAIN("remote-targets")
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage");
    qputenv("SPOOL_CREDENTIAL_STORE_DIR", directory.filePath("credentials").toUtf8());
    DatabaseManager database;
    require(database.initialize(directory.filePath("cache.sqlite")), "database opens");
    auto package = ProviderFixture::package("fixture.remote");
    auto manifest = QJsonDocument::fromJson(package.files.value("manifest.json")).object();
    manifest.insert("capabilities", QJsonArray { "remoteTargets", "originGrants" });
    package.files["manifest.json"] = QJsonDocument(manifest).toJson();
    package.manifest = *ProviderManifest::parse(package.files.value("manifest.json"));
    QFile script(SpoolTests::fixturePath("tests/providers/fixtures/remote-targets.mjs"));
    require(script.open(QIODevice::ReadOnly), "remote fixture opens");
    package.files["logic/provider.mjs"] = script.readAll();
    const auto installs = directory.filePath("providers");
    require(ProviderPackage::install(package, installs).has_value(), "remote fixture installs");
    ProviderRegistry registry(&database);
    registry.setRuntimeEnvironment({ { "id", "self-device" } }, {});
    registry.setInstallDirectory(installs);
    registry.loadModules();
    SourceHub hub(&registry);
    QCoro::waitFor(registry.restore());
    QStringList accounts;
    for (int n = 0; n < 5; ++n) {
        const QString account = registry.finishSetup({},
            { { "module", "fixture.remote" }, { "account", QString::number(n) },
                { "label", QStringLiteral("Account %1").arg(n) }, { "configuration", QVariantMap() } });
        registry.useAccount(account);
        waitUntil([&] { return hub.source(account) != nullptr; }, "remote source starts");
        accounts.append(account);
        QCoro::waitFor(hub.call(account, "setup", { { "listDelay", n == 0 ? 300 : 30 } }));
    }
    const QString account = accounts.first();
    const QString targetA = hub.scoped(account, "a");
    const QString targetB = hub.scoped(account, "b");
    const auto stats = [&] { return QCoro::waitFor(hub.call(account, "stats")); };
    RemoteTargetsController remote(&hub, &registry, nullptr);
    require(remote.available() && remote.targets().first().toMap().value("isLocal").toBool()
            && remote.selectedTargetId().isEmpty(),
        "outbound support exposes local-first selection without restoring a target");
    const auto localSelection = remote.selection();
    int listingActive = 0;
    int listingMaximum = 0;
    const auto listEvents = QObject::connect(
        &hub, &SourceHub::accountEvent, [&](const QString&, const QString& type, const QVariantMap& payload) {
            if (type == "fixtureRemoteList") {
                listingActive += payload.value("active").toBool() ? 1 : -1;
                listingMaximum = std::max(listingMaximum, listingActive);
            }
        });
    remote.setChooserVisible(true);
    waitUntil([&] { return remote.targets().size() > 1; }, "fast accounts publish progressively");
    require(remote.busy() && stats().value("lists").toInt() == 1,
        "one slow discovery does not hold fast account results behind it");
    waitUntil([&] { return !remote.busy(); }, "all discovery completes");
    require(remote.targets().size() == 11, "self target is excluded separately for each account");
    for (const auto& id : accounts)
        require(QCoro::waitFor(hub.call(id, "stats")).value("lists").toInt() == 1,
            "each eligible account participates in bounded discovery");
    require(listingMaximum == 4 && listingActive == 0,
        "discovery uses at most four concurrent account requests and eventually drains all five");
    QObject::disconnect(listEvents);
    remote.setChooserVisible(false);
    const auto select = [&](const QString& target = QString()) {
        remote.selectTarget(target.isEmpty() ? targetA : target);
        waitUntil([&] { return !remote.busy(); }, "selection connects");
        require(!remote.selectedTargetId().isEmpty(), "selection succeeds");
    };
    const auto setup = [&](QVariantMap options = {}) {
        remote.disconnectTarget();
        remote.setQueueVisible(false);
        remote.setChooserVisible(false);
        remote.setForeground(true);
        QCoro::waitFor(hub.call(account, "setup", options));
        remote.setChooserVisible(true);
        waitUntil([&] { return !remote.busy(); }, "fresh discovery settles");
        remote.setChooserVisible(false);
    };
    const auto configure = [&](QVariantMap options) { QCoro::waitFor(hub.call(account, "configure", options)); };
    const auto publish = [&](QVariantMap options = {}) { QCoro::waitFor(hub.call(account, "publish", options)); };
    const auto settledCommand = [&] { waitUntil([&] { return !remote.busy(); }, "command settles"); };
    select();
    require(!remote.isCurrent(localSelection) && stats().value("commands").toList().isEmpty(),
        "selection invalidates a prospective local handoff but does not play or transfer media");
    require(remote.state().value("item").toMap().value("id") == hub.scoped(account, "a-movie")
            && !remote.state().contains("volume"),
        "media IDs are account scoped and unknown volume is not fabricated");
    QString observedTarget;
    const auto remoteEvents = QObject::connect(
        &hub, &SourceHub::accountEvent, [&](const QString& owner, const QString& type, const QVariantMap& payload) {
            if (owner == account && type == "remoteChanged")
                observedTarget = payload.value("targetId").toString();
        });
    publish({ { "targetId", "b" } });
    waitUntil([&] { return observedTarget == targetB; }, "outbound events cross the hub with scoped target identity");
    QCoro::waitFor(hub.call(accounts[1], "publish", { { "targetId", "a" } }));
    require(stats().value("stateReads").toList().isEmpty(),
        "another target or another account cannot invalidate the selected target");
    QObject::disconnect(remoteEvents);
    const auto selected = remote.selection();
    require(!QCoro::waitFor(remote.play(
                selected, { hub.scoped(account, "movie"), hub.scoped(accounts[1], "movie") }, 0, "0", "now"))
            && stats().value("commands").toList().isEmpty(),
        "mixed-account play is rejected before a backend request or queue mutation");
    require(QCoro::waitFor(remote.play(selected, { hub.scoped(account, "movie"), hub.scoped(account, "movie") }, 1,
                "9007199254740993", "now")),
        "accepted same-account remote start returns true");
    const auto playCommand = stats().value("commands").toList().last().toMap();
    require(playCommand.value("itemIds").toStringList() == QStringList({ "movie", "movie" })
            && playCommand.value("positionTicks").toString() == "9007199254740993"
            && playCommand.value("index").toInt() == 1,
        "outbound play preserves duplicate occurrences, raw IDs and exact ticks");
    require(!QCoro::waitFor(remote.play(selected, { hub.scoped(account, "movie") }, 0, "9223372036854775808", "now"))
            && stats().value("commands").toList().size() == 1,
        "overflow ticks are rejected without HTTP/provider command work");
    remote.send({ { "action", "audioTrack" }, { "trackId", QVariant::fromValue(nullptr) } });
    settledCommand();
    require(stats().value("commands").toList().size() == 1, "null audio selection is rejected");
    remote.send({ { "action", "subtitleTrack" }, { "trackId", QVariant::fromValue(nullptr) } });
    settledCommand();
    require(stats().value("commands").toList().last().toMap().value("trackId").isNull(),
        "subtitle Off remains explicit null at the provider boundary");
    QPointer<ProviderUiContext> advanced;
    const auto pickerEvent = QObject::connect(&registry, &ProviderRegistry::componentRequested,
        [&](QObject *context) { advanced = qobject_cast<ProviderUiContext *>(context); });
    remote.openAdvancedControls();
    waitUntil([&] { return !advanced.isNull(); }, "advanced controls request a real provider picker presentation");
    remote.selectTarget(targetB);
    waitUntil([&] { return !remote.busy(); }, "target switch closes old controls");
    require(!advanced || advanced->closed(),
        "changing target cancels the old provider controls context and its pending operations");
    QObject::disconnect(pickerEvent);

    setup({ { "connectDelay", 150 } });
    remote.selectTarget(targetA);
    remote.selectTarget(targetB);
    waitUntil([&] { return !remote.busy(); }, "replacement selection completes");
    require(remote.selectedTargetId() == targetB && remote.state().value("title") == "b",
        "an old target connection cannot overwrite a newer target state");
    require(!QCoro::waitFor(remote.play(selected, { hub.scoped(account, "movie") }, 0, "0", "now")),
        "captured stale selection cannot start media");

    setup({ { "stateDelay", 150 }, { "ignoreCommands", true } });
    select();
    publish();
    waitUntil([&] { return stats().value("stateReads").toList().size() == 1; }, "old snapshot is in flight");
    remote.send({ { "action", "seek" }, { "positionTicks", "4000000000" } });
    settledCommand();
    remote.send({ { "action", "pause" } });
    settledCommand();
    elapse(400);
    require(remote.positionTicks() == "4000000000" && remote.state().value("state") == "paused",
        "optimistic seek and pause both survive pre-command and stale post-command snapshots");
    publish({ { "state", QVariantMap { { "state", "paused" }, { "positionTicks", "4000000000" } } } });
    elapse(250);
    configure({ { "stateDelay", 0 } });
    publish({ { "state", QVariantMap { { "state", "playing" }, { "positionTicks", "4100000000" } } } });
    waitUntil([&] { return remote.positionTicks() == "4100000000" && remote.state().value("state") == "playing"; },
        "backend confirmation releases optimism for later genuine changes");

    setup({ { "ack", true }, { "ignoreCommands", true }, { "staleAck", true } });
    select();
    remote.send({ { "action", "pause" } });
    settledCommand();
    publish();
    elapse(100);
    require(remote.state().value("state") == "paused",
        "old controller acknowledgements cannot roll back a pending command");
    configure({ { "staleAck", false } });
    publish();
    waitUntil([&] { return remote.state().value("state") == "playing"; },
        "genuine command sequence acknowledgement ends optimism");

    setup({ { "ignoreCommands", true } });
    select();
    remote.setControlsVisible(true);
    remote.send({ { "action", "pause" } });
    settledCommand();
    waitUntil([&] { return remote.state().value("state") == "playing"; },
        "an ignored command yields to target truth after the bounded confirmation window", 11000);
    remote.setControlsVisible(false);

    setup();
    select();
    require(stats().value("queueReads").toList().isEmpty(), "closed queues are never fetched during connection");
    remote.setQueueVisible(true);
    waitUntil([&] { return !remote.queueBusy(); }, "first queue page completes");
    require(entries(remote) == QStringList({ "first", "second" }) && remote.queueHasMore(),
        "queue pages preserve duplicate media as distinct occurrences");
    remote.requestQueuePage(false);
    waitUntil([&] { return !remote.queueBusy(); }, "continuation queue page completes");
    require(entries(remote) == QStringList({ "first", "second", "third" }) && !remote.queueHasMore(),
        "advancing empty pages do not terminate a queue");
    remote.send({ { "action", "queueRemove" }, { "entryId", "second" } });
    settledCommand();
    waitUntil([&] { return !remote.queueBusy() && entries(remote) == QStringList({ "first", "third" }); },
        "removing second occurrence preserves first and refreshes authoritative entries");
    const int queueReads = stats().value("queueReads").toList().size();
    publish();
    elapse(100);
    require(stats().value("queueReads").toList().size() == queueReads,
        "position/state polls do not refetch a queue whose genuine revision is unchanged");
    publish({ { "revision", true } });
    waitUntil([&] { return stats().value("queueReads").toList().size() > queueReads && !remote.queueBusy(); },
        "a real queue revision refreshes the open queue");
    remote.setQueueVisible(false);
    const int closedReads = stats().value("queueReads").toList().size();
    publish({ { "revision", true } });
    elapse(100);
    require(stats().value("queueReads").toList().size() == closedReads,
        "even a changed revision does not load a closed queue");
    setup({ { "uncertain", true } });
    select();
    remote.setQueueVisible(true);
    waitUntil([&] { return !remote.queueBusy(); }, "queue loads before uncertain mutation");
    remote.send({ { "action", "queueRemove" }, { "entryId", "second" } });
    settledCommand();
    waitUntil([&] { return !remote.queueBusy() && entries(remote) == QStringList({ "first", "third" }); },
        "an uncertain committed mutation is read back rather than rolled back");
    require(stats().value("commands").toList().size() == 1 && !remote.problem().isEmpty(),
        "an uncertain mutation is never automatically repeated or reported as confirmed");
    setup();
    select();
    remote.setQueueVisible(true);
    waitUntil([&] { return !remote.queueBusy(); }, "queue loads before movement");
    remote.send({ { "action", "queueMove" }, { "entryId", "second" }, { "index", 0 },
        { "afterEntryId", QVariant::fromValue(nullptr) } });
    settledCommand();
    waitUntil([&] { return !remote.queueBusy() && entries(remote) == QStringList({ "second", "first" }); },
        "movement identifies the exact duplicate occurrence and uses the post-removal first-position anchor");
    remote.send({ { "action", "queueMove" }, { "entryId", "first" }, { "index", 0 }, { "afterEntryId", "second" } });
    settledCommand();
    require(stats().value("commands").toList().size() == 1,
        "an inconsistent destination index and anchor never reach the backend");
    setup({ { "unsupported", true } });
    select();
    remote.send({ { "action", "pause" } });
    settledCommand();
    require(
        stats().value("commands").toList().isEmpty(), "missing target capabilities reject commands before dispatch");
    setup({ { "failPlay", true } });
    select();
    require(!QCoro::waitFor(remote.play(remote.selection(), { hub.scoped(account, "movie") }, 0, "0", "now")),
        "unsupported delegated playback cannot report an accepted start");
    waitUntil([&] { return !remote.state().value("commands").toStringList().contains("play"); },
        "failed play refreshes current peer capabilities");
    require(remote.selectedTargetId() == targetA && !remote.problem().isEmpty(),
        "remote_play_unavailable keeps an otherwise supported peer attached with a nonfatal problem");
    remote.send({ { "action", "pause" } });
    settledCommand();
    waitUntil([&] { return remote.state().value("state") == "paused"; },
        "supported transport remains usable after delegated playback is unavailable");
    setup({ { "unknownFields", true } });
    select();
    require(!remote.state().contains("positionTicks") && !remote.state().contains("runtimeTicks")
            && !remote.state().contains("volume") && !remote.state().contains("preview"),
        "undefined optional provider fields remain unknown rather than rejecting a valid target or fabricating values");
    for (const auto& mode : { "badCursor", "badEntries", "forever" }) {
        setup({ { QLatin1String(mode), true } });
        select();
        remote.setQueueVisible(true);
        waitUntil([&] { return !remote.queueBusy(); }, "invalid queue page fails boundedly");
        require(remote.queue().isEmpty() && !remote.problem().isEmpty(),
            "invalid cursors/occurrences cannot leave editable stale queue entries");
        if (QByteArray(mode) == "forever")
            require(stats().value("queueReads").toList().size() == 256,
                "advancing empty remote queue pages hit the collector ceiling");
    }
    setup({ { "noRevision", true } });
    select();
    remote.setQueueVisible(true);
    waitUntil([&] { return !remote.queueBusy(); }, "revisionless queue opens");
    const int initialReads = stats().value("queueReads").toList().size();
    publish({ { "events", 20 } });
    elapse(250);
    require(stats().value("queueReads").toList().size() == initialReads,
        "revisionless state invalidations do not amplify into queue requests");
    waitUntil([&] { return stats().value("queueReads").toList().size() > initialReads; },
        "an open revisionless queue refreshes on its five-second cadence", 6500);
    remote.setQueueVisible(false);
    remote.setForeground(false);
    const auto background = stats();
    publish({ { "events", 20 } });
    elapse(3200);
    require(stats().value("stateReads") == background.value("stateReads")
            && stats().value("queueReads") == background.value("queueReads"),
        "background state stops poll and queue work, including pushed invalidations");
    remote.setForeground(true);
    waitUntil(
        [&] { return stats().value("stateReads").toList().size() > background.value("stateReads").toList().size(); },
        "foreground resumes the selected target only");

    setup({ { "origin", "http://127.0.0.1:23456" } });
    remote.selectTarget(targetA);
    waitUntil([&] { return !registry.networkConsent().isEmpty(); }, "target origin consent opens before connect");
    require(stats().value("connects").toList().isEmpty()
            && registry.networkConsent().value("origin") == "http://127.0.0.1:23456"
            && registry.networkConsent().value("unencrypted").toBool(),
        "no peer is contacted before exact account origin consent with HTTP warning");
    registry.resolveNetworkConsent(registry.networkConsent().value("id").toString(), false);
    waitUntil([&] { return !remote.busy(); }, "denied target selection settles");
    require(remote.selectedTargetId().isEmpty() && stats().value("connects").toList().isEmpty(),
        "denial leaves target detached and performs no remote connection");
    remote.selectTarget(targetA);
    waitUntil([&] { return !registry.networkConsent().isEmpty(); }, "target origin can be explicitly retried");
    const auto staleConsent = registry.networkConsent().value("id").toString();
    remote.disconnectTarget();
    registry.resolveNetworkConsent(staleConsent, true);
    elapse(50);
    require(registry.networkConsent().isEmpty() && stats().value("connects").toList().isEmpty(),
        "detaching cancels outstanding consent and late approval cannot connect");
    remote.selectTarget(targetA);
    waitUntil([&] { return !registry.networkConsent().isEmpty(); }, "fresh selection requests origin");
    registry.resolveNetworkConsent(registry.networkConsent().value("id").toString(), true);
    waitUntil([&] { return !remote.busy(); }, "approved target connects without source restart");
    require(remote.selectedTargetId() == targetA
            && registry.accountOriginAllowed(account, QUrl("http://127.0.0.1:23456/image"))
            && !registry.accountOriginAllowed(account, QUrl("https://never-grant.invalid/image")),
        "only the selected preferred origin is granted, not every discovered address");
    configure({ { "preview", "http://127.0.0.1:23456/tile/{index}.jpg" } });
    const auto approvedPreview = QCoro::waitFor(hub.remoteState(targetA, false, "preview-check"));
    require(approvedPreview.contains("preview"), "approved-origin numeric tile descriptors are exposed");
    configure({ { "preview", "https://never-grant.invalid/tile/{index}.jpg" } });
    require(failure(hub.remoteState(targetA, false, "preview-check")) == "invalid_remote_response",
        "unapproved previews are rejected before rendering a network image");
    configure({ { "preview", "http://127.0.0.{index}:23456/tile.jpg" } });
    require(failure(hub.remoteState(targetA, false, "preview-check")) == "invalid_remote_response",
        "numeric placeholder substitution cannot change a preview authority");
    setup({ { "badTracks", true } });
    remote.selectTarget(targetA);
    waitUntil([&] { return !remote.busy(); }, "oversized tracks rejected");
    require(remote.selectedTargetId().isEmpty() && remote.state().isEmpty(),
        "a target exceeding the 128-track bound never publishes unsafe state");
    setup({ { "tooMany", true } });
    const auto boundedTargets = remote.targets();
    require(std::none_of(boundedTargets.begin(), boundedTargets.end(),
                [&](const QVariant& row) { return row.toMap().value("accountId") == account; }),
        "oversized per-account target results do not enter the chooser");

    setup();
    select();
    configure({ { "stateDelay", 150 } });
    publish({ { "events", 30 } });
    elapse(400);
    require(stats().value("stateReads").toList().size() <= 2,
        "target invalidation bursts coalesce with the pending state read");
    configure({ { "gone", true } });
    publish();
    waitUntil([&] { return remote.selectedTargetId().isEmpty(); }, "vanished target detaches");
    require(remote.state().isEmpty() && remote.queue().isEmpty() && !remote.problem().isEmpty(),
        "a disappeared target clears its models and offers explicit local selection without fallback playback");
    setup();
    select();
    QCoro::waitFor(hub.call(account, "dropCapabilities"));
    waitUntil([&] { return remote.selectedTargetId().isEmpty(); }, "capability support loss detaches");
    require(!remote.isCurrent(selected) && remote.state().isEmpty(),
        "source/capability generations invalidate captured play selections");
    for (const auto& other : accounts)
        registry.setAccountEnabled(other, false);
    remote.disconnectTarget();
    require(
        !remote.available(), "without an active negotiated account or recovery problem the outbound feature is hidden");
    return 0;
}
