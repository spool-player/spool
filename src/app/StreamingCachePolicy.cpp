#include "StreamingCachePolicy.h"

#include <algorithm>

namespace Spool {

namespace {

    constexpr qint64 kMiB = 1024LL * 1024;
    constexpr qint64 kMinForwardBytes = 16 * kMiB;
    constexpr qint64 kMinBackBytes = 8 * kMiB;
    constexpr qint64 kMaxManualForwardBytes = 4096 * kMiB;
    constexpr qint64 kAutoMaxBackBytes = 128 * kMiB;
    constexpr qint64 kAutoMaxForwardBytes = 512 * kMiB;
    // Each 1:2 growth step adds 16 MiB behind + 32 MiB forward = 48 MiB total.
    constexpr qint64 kRatioStepBackBytes = 16 * kMiB;
    constexpr qint64 kRatioStepForwardBytes = 32 * kMiB;
    constexpr qint64 kRatioStepTotalBytes = kRatioStepBackBytes + kRatioStepForwardBytes;
    // Total demuxer memory at the 1:2 knee (128 MiB behind + 256 MiB forward).
    constexpr qint64 kKneeTotalBytes = kAutoMaxBackBytes + 2 * kAutoMaxBackBytes;

} // namespace

StreamingCacheTarget computeStreamingCacheTarget(const StreamingCacheInput& input)
{
    const qint64 defaultForward = input.constrainedDevice ? 32 * kMiB : 64 * kMiB;
    const qint64 baseForward
        = std::clamp(input.configuredForwardBytes > 0 ? input.configuredForwardBytes : defaultForward, kMinForwardBytes,
            kMaxManualForwardBytes);
    const qint64 baseBack = std::clamp(baseForward / 2, kMinBackBytes, kAutoMaxBackBytes);

    if (!input.autoScaleEnabled || !input.availableMemoryKnown || input.availableMemoryBytes <= 0)
        return { baseForward, baseBack };

    const qint64 reserve = input.constrainedDevice
        ? std::clamp(input.totalMemoryBytes > 0 ? input.totalMemoryBytes / 4 : 384 * kMiB, 256 * kMiB, 512 * kMiB)
        : std::clamp(input.totalMemoryBytes > 0 ? input.totalMemoryBytes / 8 : 1024 * kMiB, 512 * kMiB, 2048 * kMiB);

    const qint64 surplus = std::max<qint64>(0, input.availableMemoryBytes - reserve);
    const qint64 usableBudget = input.constrainedDevice ? surplus / 5 : surplus / 4;

    qint64 scaledBack = 0;
    qint64 scaledForward = 0;
    if (usableBudget >= kKneeTotalBytes) {
        scaledBack = kAutoMaxBackBytes;
        const qint64 extraSteps = (usableBudget - kKneeTotalBytes) / kRatioStepForwardBytes;
        scaledForward = std::min(kAutoMaxForwardBytes, 2 * kAutoMaxBackBytes + extraSteps * kRatioStepForwardBytes);
    } else if (usableBudget >= kRatioStepTotalBytes) {
        const qint64 steps = usableBudget / kRatioStepTotalBytes;
        scaledBack = std::min(kAutoMaxBackBytes, steps * kRatioStepBackBytes);
        scaledForward = std::min(2 * kAutoMaxBackBytes, steps * kRatioStepForwardBytes);
    }

    const qint64 forwardBytes = std::max(baseForward, scaledForward);
    const qint64 backCap = std::min(kAutoMaxBackBytes, forwardBytes / 2);
    const qint64 backBytes = std::clamp(std::max(baseBack, scaledBack), kMinBackBytes, backCap);
    return { forwardBytes, backBytes };
}

StreamingCacheTarget StreamingCacheScaler::update(const StreamingCacheTarget& candidate)
{
    if (!m_initialized) {
        m_initialized = true;
        m_current = candidate;
        m_previousCandidate = candidate;
        return m_current;
    }

    // Memory pressure shrinks immediately; growth requires two consecutive samples.
    const qint64 nextForward = candidate.forwardBytes <= m_current.forwardBytes
        ? candidate.forwardBytes
        : std::max(m_current.forwardBytes, std::min(candidate.forwardBytes, m_previousCandidate.forwardBytes));
    const qint64 nextBack = candidate.backBytes <= m_current.backBytes
        ? candidate.backBytes
        : std::max(m_current.backBytes, std::min(candidate.backBytes, m_previousCandidate.backBytes));

    m_previousCandidate = candidate;
    m_current = { nextForward, std::min(nextBack, nextForward / 2) };
    return m_current;
}

void StreamingCacheScaler::reset(const StreamingCacheTarget& initial)
{
    m_initialized = true;
    m_current = initial;
    m_previousCandidate = initial;
}

} // namespace Spool
