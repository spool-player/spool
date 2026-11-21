#include "ArtworkConcurrency.h"

#include <algorithm>
#include <cmath>

namespace Spool {

namespace {

    // Open a little more than the server works on at once: enough that it
    // never waits on the network between requests, not so much that a scroll
    // leaves it a queue of posters nobody will see.
    constexpr double kHeadroom = 1.1;
    // Where a measured round trip may start the limit. Higher is earned.
    constexpr double kSeedCeiling = 8.0;
    // Fetches per bucket of the windowed minimum: the fastest is remembered
    // for between one and two buckets.
    constexpr int kBucketSamples = 60;
    constexpr double kRecentWeight = 0.25;
    constexpr double kParallelismWeight = 0.2;
    // How far each sample moves the limit toward its new target.
    constexpr double kStep = 0.3;
    constexpr int kWarmupSamples = 4;

    double minimum(double a, double b)
    {
        return a < 0 ? b : b < 0 ? a : std::min(a, b);
    }

} // namespace

void ArtworkConcurrency::setFixed(int limit)
{
    m_fixed = limit > 0 ? std::clamp(limit, 1, kMaximum) : 0;
}

int ArtworkConcurrency::limit() const
{
    if (!automatic())
        return m_fixed;
    return std::clamp(static_cast<int>(std::lround(m_limit)), kMinimum, kMaximum);
}

double ArtworkConcurrency::fastestMs() const
{
    return minimum(m_fastestCurrent, m_fastestPrevious);
}

void ArtworkConcurrency::seed(double roundTripMs)
{
    if (!(roundTripMs >= 0))
        return;
    m_roundTripMs = roundTripMs;
    // Each 10 ms of round trip is worth another request in flight; a LAN
    // starts where a fixed client would, a distant server well above it.
    if (m_samples < kWarmupSamples)
        m_limit = std::clamp(kDefault + roundTripMs / 10.0, static_cast<double>(kDefault), kSeedCeiling);
}

void ArtworkConcurrency::record(double durationMs, int inFlight)
{
    if (!(durationMs >= 0) || inFlight < 1)
        return;
    ++m_samples;
    m_fastestCurrent = minimum(m_fastestCurrent, durationMs);
    if (++m_bucketSamples >= kBucketSamples) {
        m_fastestPrevious = m_fastestCurrent;
        m_fastestCurrent = -1;
        m_bucketSamples = 0;
    }
    m_recentMs = m_recentMs < 0 ? durationMs : m_recentMs + (durationMs - m_recentMs) * kRecentWeight;

    // Little's law: of `inFlight` open, the server was really working on
    // this many, given how much longer than unqueued this fetch took.
    const double parallel = inFlight * fastestMs() / std::max(1.0, durationMs);
    m_parallelism = m_parallelism < 0 ? parallel : m_parallelism + (parallel - m_parallelism) * kParallelismWeight;
    if (m_samples < kWarmupSamples)
        return;

    // One more than that, so a server keeping up with everything sent is
    // always offered another request rather than rounding back to the same.
    double target = std::max(static_cast<double>(kMinimum), m_parallelism * kHeadroom + 1.0);
    // Growing is only evidence-based while the limit is what holds requests
    // back; a half-empty pipe says nothing about whether more would help.
    if (target > m_limit && inFlight < limit())
        target = m_limit;
    m_limit = std::clamp(
        m_limit + (target - m_limit) * kStep, static_cast<double>(kMinimum), static_cast<double>(kMaximum));
}

} // namespace Spool
