#pragma once

namespace Spool {

// How many artwork downloads to have open at once against one server.
//
// Posters are small, so a grid's worth of them is limited by round trips far
// more often than by bandwidth: on a LAN six at a time is plenty, while over
// the internet at 80 ms every extra request in flight is nearly free until
// the link or the server's image scaler fills up. The controller starts from
// a round trip estimated off the first fetches and then follows them. While they
// finish about as fast as the fastest one seen, nothing is queueing anywhere
// and more requests in flight are pure gain, so the limit grows -- but only
// while it is actually the bottleneck, so it settles at what the screen asks
// for. When fetches slow down, something is saturated and the limit backs
// off in proportion. This is the gradient form of TCP-style congestion
// control, applied to requests rather than packets.
//
// "As fast as the fastest" needs a fastest measured without queueing, and a
// busy grid never runs light on its own. So now and then, like BBR's
// PROBE_RTT, the limit dips to a few requests for a handful of fetches and
// the baseline is measured again; that is also how a move to a slower
// network stops looking like congestion.
class ArtworkConcurrency final {
public:
    static constexpr int kMinimum = 2;
    static constexpr int kMaximum = 32;
    // What a fixed setting and an unmeasured server start from.
    static constexpr int kDefault = 6;

    // Requests allowed while re-measuring the unqueued baseline.
    static constexpr int kProbeLimit = 3;

    // A positive value pins the limit; zero adapts.
    void setFixed(int limit);
    bool automatic() const
    {
        return m_fixed <= 0;
    }
    int limit() const;

    // A round trip estimated from the first fetches: where to start. Only
    // the opening fetches can move it; after that the samples decide.
    void seed(double roundTripMs);
    // A fetch that went to the network finished after `durationMs`, having
    // started with `inFlight` fetches (itself included) open to this server.
    void record(double durationMs, int inFlight);

    double roundTripMs() const
    {
        return m_roundTripMs;
    }
    double fastestMs() const
    {
        return m_fastestMs;
    }
    double recentMs() const
    {
        return m_recentMs;
    }

private:
    int m_fixed = 0;
    double m_limit = kDefault;
    double m_roundTripMs = -1;
    double m_fastestMs = -1;
    double m_recentMs = -1;
    int m_samples = 0;
    int m_untilProbe = 12;
    int m_probeSamples = 0;
    int m_probeLeft = 0;
};

} // namespace Spool
