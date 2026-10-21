#include "ArtworkConcurrency.h"

#include <algorithm>
#include <cmath>

namespace Spool {

namespace {

    // Fetches this much slower than the fastest still count as unqueued:
    // image scaling on the server varies from poster to poster, and a few
    // milliseconds of jitter on a LAN is not congestion.
    constexpr double kTolerance = 1.5;
    constexpr double kSlackMs = 8.0;
    // Fetches between baseline probes, and unqueued fetches each probe takes.
    constexpr int kProbeInterval = 80;
    constexpr int kProbeFetches = 6;
    // A probe that cannot get its fetches (the screen stopped asking) ends.
    constexpr int kProbePatience = 30;
    constexpr double kRecentWeight = 0.25;
    // How far each sample moves the limit toward its new target.
    constexpr double kStep = 0.3;
    constexpr int kWarmupSamples = 4;

} // namespace

void ArtworkConcurrency::setFixed(int limit)
{
    m_fixed = limit > 0 ? std::clamp(limit, 1, kMaximum) : 0;
}

int ArtworkConcurrency::limit() const
{
    if (!automatic())
        return m_fixed;
    const int limit = std::clamp(static_cast<int>(std::lround(m_limit)), kMinimum, kMaximum);
    return m_probeLeft > 0 ? std::min(limit, kProbeLimit) : limit;
}

void ArtworkConcurrency::seed(double roundTripMs)
{
    if (!(roundTripMs >= 0))
        return;
    m_roundTripMs = roundTripMs;
    // Each 10 ms of round trip is worth another request in flight; a LAN
    // starts where a fixed client would, a distant server well above it.
    if (m_samples < kWarmupSamples)
        m_limit = std::clamp(kDefault + roundTripMs / 10.0, static_cast<double>(kDefault), 24.0);
}

void ArtworkConcurrency::record(double durationMs, int inFlight)
{
    if (!(durationMs >= 0))
        return;
    ++m_samples;
    if (m_probeLeft > 0) {
        // Only fetches that started light speak for the unqueued baseline;
        // ones still finishing from before the dip are ignored. None of them
        // say what the full limit costs, so the recent average waits.
        if (inFlight <= kProbeLimit) {
            m_fastestMs = m_fastestMs < 0 ? durationMs : std::min(m_fastestMs, durationMs);
            ++m_probeSamples;
        }
        if (--m_probeLeft <= 0 || m_probeSamples >= kProbeFetches) {
            m_probeLeft = 0;
            m_untilProbe = kProbeInterval;
        }
        return;
    }

    m_recentMs = m_recentMs < 0 ? durationMs : m_recentMs + (durationMs - m_recentMs) * kRecentWeight;

    m_fastestMs = m_fastestMs < 0 ? durationMs : std::min(m_fastestMs, durationMs);
    if (automatic() && --m_untilProbe <= 0) {
        // Start a fresh window: the next light fetches set the baseline.
        m_probeLeft = kProbePatience;
        m_probeSamples = 0;
        m_fastestMs = -1;
        return;
    }
    if (m_samples < kWarmupSamples)
        return;

    const double expected = m_fastestMs * kTolerance + kSlackMs;
    const double gradient = std::clamp(expected / std::max(1.0, m_recentMs), 0.5, 1.0);
    // A sharp slowdown is either overload or a slower network; only a fresh
    // baseline can tell which, so it comes sooner.
    if (gradient < 0.6)
        m_untilProbe = std::min(m_untilProbe, 10);
    // Growing is only evidence-based while the limit is what holds requests
    // back; a half-empty pipe says nothing about whether more would help.
    const bool saturated = inFlight >= limit();
    const double headroom = saturated && gradient >= 1.0 ? std::sqrt(m_limit) : 0.0;
    const double target = m_limit * gradient + headroom;
    m_limit = std::clamp(
        m_limit + (target - m_limit) * kStep, static_cast<double>(kMinimum), static_cast<double>(kMaximum));
}

} // namespace Spool
