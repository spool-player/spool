#pragma once

#include <QtGlobal>

namespace Spool {

struct StreamingCacheTarget {
    qint64 forwardBytes = 32LL * 1024 * 1024;
    qint64 backBytes = 16LL * 1024 * 1024;

    bool operator==(const StreamingCacheTarget& other) const = default;
};

struct StreamingCacheInput {
    qint64 totalMemoryBytes = 0;
    qint64 availableMemoryBytes = 0;
    bool availableMemoryKnown = false;
    bool constrainedDevice = false;
    bool autoScaleEnabled = true;
    // Configured user floor for forward cache (0 uses platform default floor).
    qint64 configuredForwardBytes = 0;
};

// Computes demuxer forward/backward cache targets based on free system memory.
// Scales smoothly at a 1:2 behind:forward growth ratio up to 128 MiB behind
// and 256 MiB forward, then continues scaling forward up to 512 MiB while
// keeping generous OS/application headroom reserved.
StreamingCacheTarget computeStreamingCacheTarget(const StreamingCacheInput& input);

struct StreamingMemoryAllocation {
    StreamingCacheTarget cache;
    qint64 curlGrantBytes = 0;
    qint64 committedExcessBytes = 0;
};

// Existing commitments remain charged while a smaller limit drains. The user
// floor is separate from this shared allowance for automatic cache/curl growth.
StreamingMemoryAllocation allocateStreamingMemory(StreamingCacheTarget baseline, StreamingCacheTarget desired,
    qint64 allowanceBytes, qint64 committedCacheExtraBytes, qint64 committedCurlExtraBytes, qint64 desiredCurlBytes);

// Applies pressure-safe hysteresis to live cache budget updates:
// memory pressure shrinks immediately; growth requires two consecutive samples.
class StreamingCacheScaler final {
public:
    StreamingCacheTarget update(const StreamingCacheTarget& candidate);
    void reset(const StreamingCacheTarget& initial);
    bool initialized() const
    {
        return m_initialized;
    }
    StreamingCacheTarget current() const
    {
        return m_current;
    }

private:
    bool m_initialized = false;
    StreamingCacheTarget m_current {};
    StreamingCacheTarget m_previousCandidate {};
};

} // namespace Spool
