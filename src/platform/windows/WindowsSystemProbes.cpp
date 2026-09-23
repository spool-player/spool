#include "platform/PlatformSystemProbes.h"

#include <QByteArray>

#include <windows.h>

namespace Spool {
namespace {
    constexpr qint64 kMiB = 1024LL * 1024LL;

    int physicalCoreCount()
    {
        DWORD bytes = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0)
            return 0;
        QByteArray storage(static_cast<qsizetype>(bytes), Qt::Uninitialized);
        auto *cursor = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data());
        if (!GetLogicalProcessorInformationEx(RelationProcessorCore, cursor, &bytes))
            return 0;
        int cores = 0;
        DWORD offset = 0;
        while (offset < bytes) {
            auto *entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data() + offset);
            ++cores;
            offset += entry->Size;
        }
        return cores;
    }
}

PlatformCpuProbe platformCpuProbe(int logicalCpus)
{
    const int physical = physicalCoreCount();
    return { physical > 0 ? physical : logicalCpus, 0,
        physical > 0 ? QStringLiteral("GetLogicalProcessorInformationEx") : QStringLiteral("hardware_concurrency") };
}

PlatformMemoryPolicy platformMemoryPolicy()
{
    MEMORYSTATUSEX status {};
    status.dwLength = sizeof(status);
    const qint64 total = GlobalMemoryStatusEx(&status) ? static_cast<qint64>(status.ullTotalPhys) : 0;
    return { total, 4LL * 1024LL * kMiB, 256LL * kMiB, 256LL * kMiB, 32, 24LL * kMiB, 96LL * kMiB };
}

QString platformProcessMemoryDiagnostics()
{
    return {};
}

} // namespace Spool
