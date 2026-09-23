#include "app/GroupPlaybackController.h"

#include "TestMain.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace Spool;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(1);
}

bool near(double value, double expected, double tolerance = 0.001)
{
    return std::abs(value - expected) <= tolerance;
}

} // namespace

// The timing policy watching together runs on, whichever provider hosts the
// group: when drift is corrected by rate or by seeking, and when a queue
// handoff or a seek may unpause the group.
SPOOL_TEST_MAIN("group-playback-policy")
{
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
    return 0;
}
