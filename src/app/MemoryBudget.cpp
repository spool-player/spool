#include "MemoryBudget.h"
#include "../platform/PlatformSystemProbes.h"

#include <algorithm>

namespace Spool {

MemoryBudget MemoryBudget::detect()
{
    const PlatformMemoryPolicy policy = platformMemoryPolicy();
    MemoryBudget budget;
    budget.memTotalBytes = policy.totalBytes;
    const qint64 effectiveMem = budget.memTotalBytes > 0 ? budget.memTotalBytes : policy.fallbackBytes;
    const bool lowMemory = effectiveMem < policy.lowMemoryBytes;

    budget.networkDiskCacheBytes = lowMemory ? policy.baseNetworkDiskCacheBytes / 2 : policy.baseNetworkDiskCacheBytes;
    budget.qmlImageDiskCacheBytes
        = lowMemory ? policy.baseQmlImageDiskCacheBytes / 2 : policy.baseQmlImageDiskCacheBytes;
    budget.artworkByteCacheBytes = static_cast<int>(
        std::clamp(effectiveMem / policy.artworkDivisor, policy.minimumArtworkBytes, policy.maximumArtworkBytes));
    budget.mpvDemuxerMaxBytes = lowMemory ? QByteArrayLiteral("32M") : QByteArrayLiteral("64M");
    budget.mpvDemuxerMaxBackBytes = lowMemory ? QByteArrayLiteral("16M") : QByteArrayLiteral("32M");
    return budget;
}

} // namespace Spool
