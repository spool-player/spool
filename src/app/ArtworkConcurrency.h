#pragma once

namespace Spool {

// How many artwork downloads to have open at once against one server.
//
// What matters is how soon a screenful of posters arrives, which is
// throughput, not how fast any single one comes back. Every server has some
// number of requests it really works on at once -- one per image-scaler
// thread, say -- and requests beyond that wait in its queue. Little's law
// gives that number from the fetches themselves: a fetch that took twice
// the unqueued time while eight were open was sharing the server with about
// four others. Keeping a little more than that open keeps the server busy
// without piling up a queue: a scroll cancels requests, but the server still
// does their work, and that backlog is what the next screen waits behind.
//
// On a server that is only far away, every fetch takes the round trip and no
// longer, so the estimate is simply "everything open" and the limit grows,
// while the limit is what holds the screen back, until it covers the screen.
// The unqueued time is the fastest fetch over the last hundred or so; every
// burst starts with a few fetches that had the server to themselves, so no
// probing is needed to find it, and the window lets a network change through.
class ArtworkConcurrency final {
public:
    static constexpr int kMinimum = 2;
    static constexpr int kMaximum = 32;
    // What a fixed setting and an unmeasured server start from.
    static constexpr int kDefault = 6;

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
    // A fetch that went to the network finished after `durationMs`, with as
    // many as `inFlight` fetches (itself included) open to this server while
    // it ran.
    void record(double durationMs, int inFlight);

    double roundTripMs() const
    {
        return m_roundTripMs;
    }
    double fastestMs() const;
    double recentMs() const
    {
        return m_recentMs;
    }
    // Requests the server is estimated to work on at once.
    double serverParallelism() const
    {
        return m_parallelism;
    }

private:
    int m_fixed = 0;
    double m_limit = kDefault;
    double m_roundTripMs = -1;
    double m_recentMs = -1;
    double m_parallelism = -1;
    // Windowed minimum: the fastest of this bucket and the one before.
    double m_fastestCurrent = -1;
    double m_fastestPrevious = -1;
    int m_bucketSamples = 0;
    int m_samples = 0;
};

} // namespace Spool
