#pragma once

#include "StreamingCachePolicy.h"

#include <QtGlobal>

namespace Spool {

struct MemoryBudget {
    qint64 memTotalBytes = 0;
    qint64 networkDiskCacheBytes = 0;
    qint64 qmlImageDiskCacheBytes = 0;
    int artworkByteCacheBytes = 0;
    StreamingCacheTarget demuxerCache;

    static MemoryBudget detect();
};

} // namespace Spool
