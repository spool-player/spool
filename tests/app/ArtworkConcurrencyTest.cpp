#include "app/ArtworkConcurrency.h"

#include "TestMain.h"

#include <QDebug>

#include <algorithm>
#include <cstdlib>
#include <vector>

using Spool::ArtworkConcurrency;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    qCritical() << message;
    std::exit(EXIT_FAILURE);
}

// A server `roundTripMs` away whose image scaler runs `workers` requests at
// once for `serviceMs` each, first come first served, asked for `demand`
// posters a screen. Returns how long the last screen took to fill.
struct Server {
    double roundTripMs = 0;
    double serviceMs = 0;
    int workers = 1;
    int demand = 1;
};

double fillScreens(ArtworkConcurrency& control, const Server& server, int screens)
{
    double now = 0;
    double lastFill = 0;
    std::vector<double> freeAt(static_cast<size_t>(server.workers), 0.0);
    for (int screen = 0; screen < screens; ++screen) {
        const double began = now;
        int pending = server.demand;
        struct Fetch {
            double done;
            double started;
            int peakOpen;
        };
        std::vector<Fetch> open;
        while (pending > 0 || !open.empty()) {
            while (pending > 0 && static_cast<int>(open.size()) < control.limit()) {
                --pending;
                auto worker = std::min_element(freeAt.begin(), freeAt.end());
                const double start = std::max(now + server.roundTripMs / 2, *worker);
                *worker = start + server.serviceMs;
                open.push_back({ *worker + server.roundTripMs / 2, now, 0 });
                for (Fetch& fetch : open)
                    fetch.peakOpen = std::max(fetch.peakOpen, static_cast<int>(open.size()));
            }
            const auto first = std::min_element(
                open.begin(), open.end(), [](const Fetch& a, const Fetch& b) { return a.done < b.done; });
            const Fetch fetch = *first;
            open.erase(first);
            now = fetch.done;
            control.record(now - fetch.started, fetch.peakOpen);
        }
        lastFill = now - began;
        now += 1000;
    }
    return lastFill;
}

// The viewer scrolls on every `dwellMs` before the screen has filled, then
// stops. Cancelled fetches report nothing, but the server still does their
// work, so whatever was sent past its capacity is a queue the final screen
// waits behind. Returns how long that final screen took.
double scrollThenStop(ArtworkConcurrency& control, const Server& server, int scrolls, double dwellMs)
{
    double now = 0;
    std::vector<double> freeAt(static_cast<size_t>(server.workers), 0.0);
    struct Fetch {
        double done;
        double started;
        int peakOpen;
    };
    for (int scroll = 0;; ++scroll) {
        const bool last = scroll == scrolls;
        const double began = now;
        const double leaveAt = last ? 1e18 : now + dwellMs;
        int pending = server.demand;
        std::vector<Fetch> open;
        while (pending > 0 || !open.empty()) {
            while (pending > 0 && static_cast<int>(open.size()) < control.limit()) {
                --pending;
                auto worker = std::min_element(freeAt.begin(), freeAt.end());
                const double start = std::max(now + server.roundTripMs / 2, *worker);
                *worker = start + server.serviceMs;
                open.push_back({ *worker + server.roundTripMs / 2, now, 0 });
                for (Fetch& fetch : open)
                    fetch.peakOpen = std::max(fetch.peakOpen, static_cast<int>(open.size()));
            }
            const auto first = std::min_element(
                open.begin(), open.end(), [](const Fetch& a, const Fetch& b) { return a.done < b.done; });
            if (first->done > leaveAt) {
                now = leaveAt;
                break;
            }
            const Fetch fetch = *first;
            open.erase(first);
            now = fetch.done;
            control.record(now - fetch.started, fetch.peakOpen);
        }
        if (last)
            return now - began;
    }
}

// The fastest any fixed number of downloads fills a screen of this server.
double bestFixedFill(const Server& server)
{
    double best = 1e18;
    for (int limit = 1; limit <= ArtworkConcurrency::kMaximum; ++limit) {
        ArtworkConcurrency fixed;
        fixed.setFixed(limit);
        best = std::min(best, fillScreens(fixed, server, 2));
    }
    return best;
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
    require(distant.limit() > lan.limit(), "a long round trip starts with more in flight");

    // Only far away: every extra request is free, so the limit covers the
    // screen and fills it nearly as fast as anything could.
    const Server far { 120, 15, 64, 28 };
    ArtworkConcurrency latencyBound;
    latencyBound.seed(120);
    const double farFill = fillScreens(latencyBound, far, 12);
    require(latencyBound.limit() >= 24, "a latency-bound server is given as many as the screen wants");
    require(farFill <= bestFixedFill(far) * 1.25, "a latency-bound screen fills close to the best possible");

    // A busy scaler: beyond six at once requests only queue. The limit stays
    // above six, so the server never idles, and well short of the maximum.
    const Server busy { 10, 300, 6, 40 };
    ArtworkConcurrency capacityBound;
    capacityBound.seed(10);
    const double busyFill = fillScreens(capacityBound, busy, 12);
    require(capacityBound.limit() >= 6 && capacityBound.limit() <= 16, "a capacity-bound server is kept just busy");
    require(busyFill <= bestFixedFill(busy) * 1.15, "a capacity-bound screen fills close to the best possible");

    // Both at once, as a Plex server over the internet is: slower per image
    // with more open, but far faster per screen than a cautious few.
    const Server remote { 200, 350, 7, 36 };
    ArtworkConcurrency remoteControl;
    remoteControl.seed(217);
    const double remoteFill = fillScreens(remoteControl, remote, 12);
    ArtworkConcurrency cautious;
    cautious.setFixed(4);
    require(remoteFill <= bestFixedFill(remote) * 1.2, "a distant busy server fills close to the best possible");
    require(remoteFill * 1.4 < fillScreens(cautious, remote, 2), "slower requests are not mistaken for overload");

    // Scrolling through that same server: sending past its capacity only
    // queues abandoned work, so the screen the viewer stops on must not wait
    // much longer than the best fixed choice would have made it.
    double bestScrolledFill = 1e18;
    for (int limit = 1; limit <= ArtworkConcurrency::kMaximum; ++limit) {
        ArtworkConcurrency fixedScroll;
        fixedScroll.setFixed(limit);
        bestScrolledFill = std::min(bestScrolledFill, scrollThenStop(fixedScroll, remote, 30, 600));
    }
    ArtworkConcurrency scrolling;
    scrolling.seed(217);
    require(scrollThenStop(scrolling, remote, 30, 600) <= bestScrolledFill * 1.3,
        "scrolling past posters does not bury the server in abandoned requests");

    // A network that gets slower is followed rather than read as congestion.
    ArtworkConcurrency moved;
    fillScreens(moved, { 2, 5, 64, 30 }, 8);
    const Server slower { 150, 20, 64, 30 };
    const double movedFill = fillScreens(moved, slower, 12);
    require(movedFill <= bestFixedFill(slower) * 1.25, "a uniformly slower network is not mistaken for overload");

    // A fixed setting is the limit, whatever the measurements say.
    ArtworkConcurrency fixed;
    fixed.setFixed(4);
    fixed.seed(200);
    fillScreens(fixed, far, 3);
    require(!fixed.automatic() && fixed.limit() == 4, "a fixed limit is not adapted");
    fixed.setFixed(0);
    require(fixed.automatic(), "zero returns to automatic");
    return 0;
}
