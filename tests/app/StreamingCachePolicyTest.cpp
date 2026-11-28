#include "app/StreamingCachePolicy.h"

#include "TestMain.h"
#include "TestRequire.h"

#include <QCoreApplication>
#include <QElapsedTimer>

#include <clocale>

extern "C" {
#include <mpv/client.h>
}

using namespace Spool;
using SpoolTests::require;

namespace {

constexpr qint64 kMiB = 1024LL * 1024;
constexpr qint64 kGiB = 1024LL * kMiB;

} // namespace

SPOOL_TEST_MAIN("streaming-cache-policy")
{
    QCoreApplication app(argc, argv);
    setlocale(LC_NUMERIC, "C");

    // 1. Unknown available memory falls back cleanly to configured/platform floor.
    {
        const auto unknownDesktop = computeStreamingCacheTarget({
            .totalMemoryBytes = 16 * kGiB,
            .availableMemoryBytes = 0,
            .availableMemoryKnown = false,
            .constrainedDevice = false,
            .autoScaleEnabled = true,
            .configuredForwardBytes = 32 * kMiB,
        });
        require(unknownDesktop.forwardBytes == 32 * kMiB && unknownDesktop.backBytes == 16 * kMiB,
            "unknown free RAM must stay at configured floor with 1:2 back ratio");

        const auto disabledScale = computeStreamingCacheTarget({
            .totalMemoryBytes = 32 * kGiB,
            .availableMemoryBytes = 24 * kGiB,
            .availableMemoryKnown = true,
            .constrainedDevice = false,
            .autoScaleEnabled = false,
            .configuredForwardBytes = 64 * kMiB,
        });
        require(disabledScale.forwardBytes == 64 * kMiB && disabledScale.backBytes == 32 * kMiB,
            "disabling extra streaming memory must keep base floor");
    }

    // 2. Constrained device (e.g. 1 GiB webOS TV) stays safely at floor under low headroom.
    {
        const auto lowTv = computeStreamingCacheTarget({
            .totalMemoryBytes = 1 * kGiB,
            .availableMemoryBytes = 250 * kMiB,
            .availableMemoryKnown = true,
            .constrainedDevice = true,
            .autoScaleEnabled = true,
            .configuredForwardBytes = 32 * kMiB,
        });
        require(lowTv.forwardBytes == 32 * kMiB && lowTv.backBytes == 16 * kMiB,
            "constrained device under reserve must not grow past base floor");
    }

    // 3. Monotonic 1:2 behind:forward growth up to 128 MiB behind / 256 MiB forward,
    //    then forward continues growing smoothly up to 512 MiB cap while back stays at 128 MiB.
    {
        StreamingCacheTarget previous { 32 * kMiB, 16 * kMiB };
        bool sawIntermediate1To2 = false;
        bool sawKnee128To256 = false;
        bool sawMax128To512 = false;
        for (qint64 availMiB = 512; availMiB <= 16384; availMiB += 128) {
            const auto target = computeStreamingCacheTarget({
                .totalMemoryBytes = 16 * kGiB,
                .availableMemoryBytes = availMiB * kMiB,
                .availableMemoryKnown = true,
                .constrainedDevice = false,
                .autoScaleEnabled = true,
                .configuredForwardBytes = 32 * kMiB,
            });
            require(target.forwardBytes >= previous.forwardBytes && target.backBytes >= previous.backBytes,
                "cache scaling must be monotonically non-decreasing with available memory");
            require(target.backBytes <= 128 * kMiB, "behind cache must never exceed 128 MiB");
            require(target.forwardBytes <= 512 * kMiB, "auto-scaled forward cache must never exceed 512 MiB");
            if (target.backBytes < 128 * kMiB) {
                require(target.backBytes * 2 == target.forwardBytes,
                    "before hitting the 128 MiB behind cap, behind:forward must stay at exact 1:2 ratio");
                if (target.backBytes > 16 * kMiB)
                    sawIntermediate1To2 = true;
            }
            if (target.backBytes == 128 * kMiB && target.forwardBytes == 256 * kMiB)
                sawKnee128To256 = true;
            if (target.backBytes == 128 * kMiB && target.forwardBytes == 512 * kMiB)
                sawMax128To512 = true;
            previous = target;
        }
        require(sawIntermediate1To2, "growth curve must visit intermediate 1:2 steps");
        require(sawKnee128To256, "growth curve must pass through 128 MiB behind / 256 MiB forward knee");
        require(sawMax128To512, "abundant free memory must reach 128 MiB behind / 512 MiB forward cap");
    }

    // 4. Huge free RAM (128 GiB) still respects 128 MiB / 512 MiB auto cap;
    //    explicit user slider override above 512 MiB is honored as floor.
    {
        const auto hugeRam = computeStreamingCacheTarget({
            .totalMemoryBytes = 128 * kGiB,
            .availableMemoryBytes = 96 * kGiB,
            .availableMemoryKnown = true,
            .constrainedDevice = false,
            .autoScaleEnabled = true,
            .configuredForwardBytes = 32 * kMiB,
        });
        require(hugeRam.backBytes == 128 * kMiB && hugeRam.forwardBytes == 512 * kMiB,
            "128 GiB machine must cap auto-scaled cache at 128 MiB behind / 512 MiB forward");

        const auto manualOverride = computeStreamingCacheTarget({
            .totalMemoryBytes = 16 * kGiB,
            .availableMemoryBytes = 12 * kGiB,
            .availableMemoryKnown = true,
            .constrainedDevice = false,
            .autoScaleEnabled = true,
            .configuredForwardBytes = 1024 * kMiB,
        });
        require(manualOverride.forwardBytes == 1024 * kMiB && manualOverride.backBytes == 128 * kMiB,
            "explicit manual forward floor above 512 MiB must be preserved while capping back at 128 MiB");
    }

    // 5. Hysteresis: growth requires two independent samples, pressure shrinks immediately.
    {
        StreamingCacheScaler scaler;
        const StreamingCacheTarget low { 32 * kMiB, 16 * kMiB };
        const StreamingCacheTarget mid { 256 * kMiB, 128 * kMiB };
        const StreamingCacheTarget high { 512 * kMiB, 128 * kMiB };

        require(scaler.update(low) == low, "initial sample commits immediately");
        require(scaler.update(high) == low, "single high-RAM spike must not grow cache immediately");
        require(scaler.update(mid) == mid, "second consecutive sample grows to min(previous, current)");
        require(scaler.update(high) == mid, "stepping further up requires another consecutive sample");
        require(scaler.update(high) == high, "sustained abundant RAM reaches 512/128 MiB");
        require(scaler.update(low) == low, "memory pressure must shrink immediately on a single sample");
    }

    // An explicit allowance covers demuxer growth and curl together, while
    // allocations still draining under pressure block further borrowing.
    {
        const StreamingCacheTarget baseline { 32 * kMiB, 16 * kMiB };
        const StreamingCacheTarget desired { 512 * kMiB, 128 * kMiB };
        for (qint64 limit : { 8 * kMiB, 32 * kMiB, 128 * kMiB }) {
            for (qint64 committedCurl : { qint64(0), 4 * kMiB, 64 * kMiB }) {
                const auto result = allocateStreamingMemory(baseline, desired, limit, 0, committedCurl, 64 * kMiB);
                const qint64 cacheExtra = result.cache.forwardBytes + result.cache.backBytes - 48 * kMiB;
                require(cacheExtra + result.curlGrantBytes <= limit,
                    "new cache and curl admissions stay within one explicit allowance");
                require(cacheExtra + committedCurl <= limit || cacheExtra == 0,
                    "existing curl commitments cannot be loaned to the cache again");
            }
        }
        const auto pressure = allocateStreamingMemory(baseline, desired, 8 * kMiB, 80 * kMiB, 20 * kMiB, 64 * kMiB);
        require(
            pressure.cache == baseline && pressure.curlGrantBytes == 0 && pressure.committedExcessBytes == 92 * kMiB,
            "pressure reports draining commitments and admits no new extras");
        const auto unknown = allocateStreamingMemory(baseline, desired, 0, 0, 0, 64 * kMiB);
        require(unknown.cache == baseline && unknown.curlGrantBytes == 0,
            "unknown headroom admits only the configured floor");
        const StreamingCacheTarget manual { 1024 * kMiB, 128 * kMiB };
        const auto floor = allocateStreamingMemory(manual, manual, 8 * kMiB, 0, 0, 64 * kMiB);
        require(floor.cache == manual && floor.curlGrantBytes == 8 * kMiB,
            "a manual floor is separate from the automatic extra allowance");
    }

    // 6. Live libmpv runtime property test: demuxer-max-bytes and demuxer-max-back-bytes
    //    must accept dynamic int64 updates on an initialized headless handle.
    {
        mpv_handle *handle = mpv_create();
        require(handle, "live demuxer scaling test needs a real mpv handle");
        require(mpv_set_option_string(handle, "config", "no") >= 0
                && mpv_set_option_string(handle, "terminal", "no") >= 0
                && mpv_set_option_string(handle, "vo", "null") >= 0 && mpv_set_option_string(handle, "ao", "null") >= 0
                && mpv_set_option_string(handle, "demuxer-max-bytes", "32M") >= 0
                && mpv_set_option_string(handle, "demuxer-max-back-bytes", "16M") >= 0 && mpv_initialize(handle) >= 0,
            "headless mpv initializes with baseline demuxer budgets");

        const auto apply = [&](int64_t forward, int64_t backward) {
            require(mpv_set_property_async(handle, 1, "demuxer-max-bytes", MPV_FORMAT_INT64, &forward) >= 0
                    && mpv_set_property_async(handle, 2, "demuxer-max-back-bytes", MPV_FORMAT_INT64, &backward) >= 0,
                "mpv queues both live demuxer budget updates");
            bool forwardApplied = false;
            bool backwardApplied = false;
            QElapsedTimer timeout;
            timeout.start();
            while (!(forwardApplied && backwardApplied) && timeout.elapsed() < 15000) {
                const auto *event = mpv_wait_event(handle, 0.1);
                if (event->event_id != MPV_EVENT_SET_PROPERTY_REPLY)
                    continue;
                require(event->error >= 0, "mpv acknowledges the live demuxer budget update");
                forwardApplied |= event->reply_userdata == 1;
                backwardApplied |= event->reply_userdata == 2;
            }
            require(forwardApplied && backwardApplied, "mpv acknowledges both live demuxer budget updates");
            int64_t readForward = 0;
            int64_t readBackward = 0;
            require(mpv_get_property(handle, "demuxer-max-bytes", MPV_FORMAT_INT64, &readForward) >= 0
                    && mpv_get_property(handle, "demuxer-max-back-bytes", MPV_FORMAT_INT64, &readBackward) >= 0
                    && readForward == forward && readBackward == backward,
                "mpv reports both acknowledged budgets");
        };
        apply(512 * kMiB, 128 * kMiB);
        apply(64 * kMiB, 32 * kMiB);

        mpv_terminate_destroy(handle);
    }

    return 0;
}
