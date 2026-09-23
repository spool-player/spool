#include "app/GroupClock.h"

#include "TestMain.h"

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

} // namespace

SPOOL_TEST_MAIN("group-clock")
{
    GroupClock clock;
    clock.addMeasurement({ 1'000, 1'060, 1'061, 1'101 });
    require(clock.ready(), "clock should be ready after one sample");
    require(clock.offsetMs() == 10.0, "clock should calculate NTP offset");
    require(clock.pingMs() == 50.0, "clock should calculate one-way ping");

    clock.addMeasurement({ 2'000, 2'025, 2'026, 2'041 });
    require(clock.offsetMs() == 5.0, "clock should retain the lowest-delay sample");
    require(clock.localDelayUntil(3'005, 3'000) == 0, "server time should convert to local time");
    require(clock.estimatePositionTicks(10'000'000, 4'000, 4'095) == 11'000'000,
        "position estimates should include elapsed server time");

    clock.reset();
    require(!clock.ready(), "reset should discard measurements");
    return 0;
}
