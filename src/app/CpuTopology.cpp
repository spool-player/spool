#include "CpuTopology.h"
#include "../platform/PlatformSystemProbes.h"

#include <algorithm>
#include <thread>

namespace Spool {

namespace {

    int environmentDecodeThreads()
    {
        bool ok = false;
        const int value = QString::fromLocal8Bit(qgetenv("SPOOL_WEBP_DECODE_THREADS")).toInt(&ok);
        return ok ? value : 0;
    }

} // namespace

CpuTopology detectCpuTopology()
{
    const int online = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    const PlatformCpuProbe probe = platformCpuProbe(online);
    // Size the pool for the cores the machine has, not the ones that happened
    // to be awake during startup. This runs once, and a parked core comes back
    // as soon as there is work for it.
    const int logical = std::max(online, probe.installedCpus);
    const int physical = std::max(1, probe.physicalCores);
    const bool smt = logical > physical;
    int decodeThreads = environmentDecodeThreads();
    if (decodeThreads <= 0)
        decodeThreads = smt ? physical : (logical + 1) / 2;
    decodeThreads = std::clamp(decodeThreads, 1, logical * 2);
    const QString source = probe.source;
    return { logical, physical, smt, decodeThreads, source };
}

} // namespace Spool
