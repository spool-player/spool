#include "app/ArtworkConcurrency.h"

#include "TestMain.h"

#include <QDebug>

#include <algorithm>
#include <cstdlib>

using Spool::ArtworkConcurrency;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    qCritical() << message;
    std::exit(EXIT_FAILURE);
}

// A server whose fetches take `baseMs` until more than `capacity` run at
// once, after which they queue and slow in proportion. Returns the highest
// limit seen over the last 50 fetches: where it has settled, probes aside.
int drive(ArtworkConcurrency& control, double baseMs, int capacity, int demand, int fetches)
{
    int settled = 0;
    for (int i = 0; i < fetches; ++i) {
        const int inFlight = std::min(control.limit(), demand);
        const double durationMs = baseMs * std::max(1.0, static_cast<double>(inFlight) / capacity);
        control.record(durationMs, inFlight);
        if (i >= fetches - 50)
            settled = std::max(settled, control.limit());
    }
    return settled;
}

} // namespace

SPOOL_TEST_MAIN("artwork-concurrency")
{
    // A distant server starts above the LAN default before any poster lands.
    ArtworkConcurrency lan;
    lan.seed(1);
    ArtworkConcurrency distant;
    distant.seed(120);
    require(lan.limit() == ArtworkConcurrency::kDefault, "a LAN starts at the default");
    require(distant.limit() > lan.limit() + 6, "a long round trip starts with more in flight");

    // Latency-bound: fetches cost the same however many run, so the limit
    // climbs to what the screen asks for and no further.
    ArtworkConcurrency latencyBound;
    latencyBound.seed(80);
    const int latencyLimit = drive(latencyBound, 90, 64, 28, 300);
    require(latencyLimit >= 26, "a latency-bound server is given as many as the screen wants");
    require(latencyLimit <= 31, "the limit does not run away past the demand");

    // Capacity-bound: beyond four at once the server queues; the limit backs
    // off to about what it can serve.
    ArtworkConcurrency capacityBound;
    capacityBound.seed(80);
    const int capacityLimit = drive(capacityBound, 40, 4, 40, 300);
    require(capacityLimit <= 8, "a saturated server gets fewer requests");
    require(capacityLimit >= ArtworkConcurrency::kMinimum, "never below the floor");

    // A network that gets slower is followed rather than read as congestion
    // forever: after the move it may grow again.
    ArtworkConcurrency moved;
    const int before = drive(moved, 5, 64, 30, 150);
    require(drive(moved, 60, 64, 30, 400) >= before, "a uniformly slower network is not mistaken for overload");

    // A fixed setting is the limit, whatever the measurements say.
    ArtworkConcurrency fixed;
    fixed.setFixed(4);
    fixed.seed(200);
    require(drive(fixed, 90, 64, 30, 100) == 4 && !fixed.automatic(), "a fixed limit is not adapted");
    fixed.setFixed(0);
    require(fixed.automatic(), "zero returns to automatic");
    return 0;
}
