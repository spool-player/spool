#include "platform/PlatformSystemProbes.h"

#include <sys/sysctl.h>

#include <algorithm>

namespace Spool {
namespace {
    constexpr qint64 kMiB = 1024LL * 1024LL;

    qint64 sysctlInteger(const char *name)
    {
        int64_t value = 0;
        size_t size = sizeof(value);
        return sysctlbyname(name, &value, &size, nullptr, 0) == 0 ? value : 0;
    }
}

PlatformCpuProbe platformCpuProbe(int logicalCpus)
{
    const int physical = static_cast<int>(sysctlInteger("hw.physicalcpu"));
    return { physical > 0 ? physical : logicalCpus, 0,
        physical > 0 ? QStringLiteral("hw.physicalcpu") : QStringLiteral("hardware_concurrency") };
}

PlatformMemoryPolicy platformMemoryPolicy()
{
    return { sysctlInteger("hw.memsize"), 4LL * 1024LL * kMiB, 256LL * kMiB, 256LL * kMiB, 32, 24LL * kMiB,
        96LL * kMiB };
}

QString platformProcessMemoryDiagnostics()
{
    return {};
}

} // namespace Spool
