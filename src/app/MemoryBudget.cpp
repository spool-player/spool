#include "MemoryBudget.h"
#include "../platform/PlatformSystemProbes.h"
#include "StreamingCachePolicy.h"

#include <algorithm>

namespace Spool {

MemoryBudget MemoryBudget::detect()
{
    const PlatformMemoryPolicy policy = platformMemoryPolicy();
    const PlatformStreamingMemoryState streamingMemory = platformStreamingMemoryState();
    MemoryBudget budget;
    budget.memTotalBytes = policy.totalBytes;
    const qint64 effectiveMem = budget.memTotalBytes > 0 ? budget.memTotalBytes : policy.fallbackBytes;
    const bool lowMemory = effectiveMem < policy.lowMemoryBytes;

    budget.networkDiskCacheBytes = lowMemory ? policy.baseNetworkDiskCacheBytes / 2 : policy.baseNetworkDiskCacheBytes;
    budget.qmlImageDiskCacheBytes
        = lowMemory ? policy.baseQmlImageDiskCacheBytes / 2 : policy.baseQmlImageDiskCacheBytes;
    budget.artworkByteCacheBytes = static_cast<int>(
        std::clamp(effectiveMem / policy.artworkDivisor, policy.minimumArtworkBytes, policy.maximumArtworkBytes));

    const StreamingCacheTarget demuxerCache = computeStreamingCacheTarget({
        .totalMemoryBytes = effectiveMem,
        .availableMemoryBytes = streamingMemory.availableBytes,
        .availableMemoryKnown = streamingMemory.available,
        .constrainedDevice = lowMemory,
        .autoScaleEnabled = true,
        .configuredForwardBytes = 0,
    });
    budget.mpvDemuxerMaxBytes = QByteArray::number(demuxerCache.forwardBytes);
    budget.mpvDemuxerMaxBackBytes = QByteArray::number(demuxerCache.backBytes);
    return budget;
}

} // namespace Spool
