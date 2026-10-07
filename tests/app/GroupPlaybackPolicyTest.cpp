#include "app/GroupPlaybackController.h"

#include "TestMain.h"
#include "TestRequire.h"
#include "../providers/ProviderFixture.h"
#include "cache/DatabaseManager.h"
#include "player/PlayQueueController.h"
#include "player/PlayerController.h"
#include "provider/ProviderRegistry.h"
#include "provider/SourceHub.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <functional>

using namespace Spool;

namespace {

using SpoolTests::require;

bool near(double value, double expected, double tolerance = 0.001)
{
    return std::abs(value - expected) <= tolerance;
}

void waitUntil(const std::function<bool()>& condition, const char *message)
{
    QElapsedTimer timeout;
    timeout.start();
    while (!condition() && timeout.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(condition(), message);
}

void currentAccountWithdrawal()
{
    QTemporaryDir directory;
    require(directory.isValid(), "group withdrawal temporary directory");
    qputenv("SPOOL_CREDENTIAL_STORE_DIR", directory.filePath(QStringLiteral("credentials")).toUtf8());
    DatabaseManager database;
    require(database.initialize(directory.filePath(QStringLiteral("cache.sqlite"))), "group withdrawal database");
    auto package = ProviderFixture::package(QStringLiteral("fixture.group-withdrawal"));
    package.files[QStringLiteral("logic/provider.mjs")] = QByteArrayLiteral(R"JS(
export function createSource(configuration, host) {
    return {
        describe() { return {capabilities: {groupPlayback: true}}; },
        groupJoin() { return {}; },
        groupSend() { return {}; },
        clock() { return {received: Date.now(), sent: Date.now()}; },
        offer(args) {
            host.emit('capabilitiesChanged', {capabilities: {groupPlayback: args.enabled}});
            return {};
        }
    };
}
)JS");
    const QString installs = directory.filePath(QStringLiteral("providers"));
    require(ProviderPackage::install(package, installs).has_value(), "group withdrawal fixture installs");
    ProviderRegistry registry(&database);
    registry.setInstallDirectory(installs);
    registry.loadModules();
    SourceHub hub(&registry);
    hub.setPlaybackActive(true);
    QCoro::waitFor(registry.restore());
    const auto add = [&](const QString& key) {
        const QString account = registry.finishSetup({},
            { { QStringLiteral("module"), package.manifest.id }, { QStringLiteral("account"), key },
                { QStringLiteral("label"), key }, { QStringLiteral("configuration"), QVariantMap {} } });
        registry.useAccount(account);
        return account;
    };
    const QString joinedAccount = add(QStringLiteral("joined"));
    const QString otherAccount = add(QStringLiteral("other"));
    waitUntil([&] { return hub.sources().size() == 2; }, "both group-capable accounts start");
    PlayerController player(nullptr, hub.playback(), nullptr, {});
    PlayQueueController queue(hub.playback());
    GroupPlaybackController group(&hub, &player, &queue);
    group.joinGroup(hub.scoped(joinedAccount, QStringLiteral("watching")));
    waitUntil([&] { return group.enabled(); }, "current account joins its group");
    group.requestUnpauseWhenReady();
    require(group.waitingForPlayback(), "joined group owns pending playback handoff");
    const auto offer = [&](const QString& account, bool enabled) {
        QCoro::waitFor(hub.call(account, QStringLiteral("offer"), { { QStringLiteral("enabled"), enabled } }));
        waitUntil([&] { return hub.source(account)->capabilities().testFlag(Provider::GroupPlayback) == enabled; },
            "account capability offer reaches the native hub");
    };
    offer(otherAccount, false);
    require(group.enabled() && group.waitingForPlayback(),
        "another account's withdrawal cannot clear the joined group's ownership");
    offer(otherAccount, true);
    offer(joinedAccount, false);
    require(group.available(), "another account keeps aggregate group availability true");
    require(!group.enabled() && group.currentGroupId().isEmpty() && !group.waitingForPlayback(),
        "current-account capability withdrawal releases active group and pending playback ownership");
}

} // namespace

// The timing policy watching together runs on, whichever provider hosts the
// group: when drift is corrected by rate or by seeking, and when a queue
// handoff or a seek may unpause the group.
SPOOL_TEST_MAIN("group-playback-policy")
{
    QCoreApplication app(argc, argv);
    require(GroupDriftPolicy::evaluate(99.0).method == GroupCorrection::Method::None,
        "drift below the speed threshold must not be corrected");
    require(GroupDriftPolicy::evaluate(-99.0).method == GroupCorrection::Method::None,
        "a small lead must not be corrected either");

    const GroupCorrection behind = GroupDriftPolicy::evaluate(200.0);
    require(behind.method == GroupCorrection::Method::Speed, "a 200 ms lag should speed up");
    require(near(behind.speed, 1.03), "a 200 ms lag should use the bounded mpv correction rate");
    require(behind.durationMs == 6'667, "the bounded correction should recover the measured lag");

    const GroupCorrection nearSeek = GroupDriftPolicy::evaluate(399.0);
    require(nearSeek.method == GroupCorrection::Method::Speed, "drift below 400 ms should speed correct");
    require(near(nearSeek.speed, 1.03), "near-threshold drift should retain the bounded rate");
    require(nearSeek.durationMs == 10'000, "the correction window must remain bounded");

    const GroupCorrection ahead = GroupDriftPolicy::evaluate(-200.0);
    require(ahead.method == GroupCorrection::Method::Speed, "a 200 ms lead should slow down");
    require(near(ahead.speed, 0.97), "a lead should use the symmetric bounded rate");
    require(ahead.durationMs == 6'667, "the bounded correction should give back the measured lead");

    require(GroupDriftPolicy::evaluate(400.0).method == GroupCorrection::Method::Skip,
        "drift at the speed ceiling must seek instead");
    require(
        GroupDriftPolicy::evaluate(-500.0).method == GroupCorrection::Method::Skip, "a large lead must seek instead");

    for (const double diffMs : { 100.0, -100.0, 200.0, -200.0, 300.0, -300.0 }) {
        const GroupCorrection correction = GroupDriftPolicy::evaluate(diffMs);
        require(correction.method == GroupCorrection::Method::Speed, "mid-range drift should speed correct");
        const double recovered = (correction.speed - 1.0) * correction.durationMs;
        require(near(recovered, diffMs, 1.0), "an unclipped speed correction must recover the measured drift");
    }

    GroupQueueHandoff handoff;
    handoff.arm();
    require(!handoff.canSend(false, false, true, true),
        "an old loaded session must not consume an unpause before the new queue update");

    handoff.observeQueueUpdate();
    require(!handoff.canSend(true, false, true, true), "queue resolution must block the unpause request");
    require(!handoff.canSend(false, true, true, true), "playback startup must block the unpause request");
    require(!handoff.canSend(false, false, true, false), "an unloaded file must block the unpause request");
    require(handoff.canSend(false, false, true, true), "the new loaded queue item should release the unpause request");

    handoff.cancel();
    require(!handoff.canSend(false, false, true, true), "a consumed or cancelled request must not be sent twice");

    GroupSeekResume seekResume;
    seekResume.arm(true);
    require(!seekResume.takeWhenReady(QStringLiteral("Waiting"), QStringLiteral("Ready")),
        "seek resume must wait for the group to finish buffering");
    require(!seekResume.takeWhenReady(QStringLiteral("Paused"), QStringLiteral("Pause")),
        "an ordinary pause must not resume playback");
    require(seekResume.takeWhenReady(QStringLiteral("Paused"), QStringLiteral("Ready")),
        "the initiating client should unpause only after every participant is ready");
    require(!seekResume.takeWhenReady(QStringLiteral("Paused"), QStringLiteral("Ready")),
        "a completed seek must request unpause only once");

    seekResume.arm(false);
    require(!seekResume.takeWhenReady(QStringLiteral("Paused"), QStringLiteral("Ready")),
        "seeking a paused group must leave it paused");
    currentAccountWithdrawal();
    return 0;
}
