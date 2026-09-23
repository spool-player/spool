#include "platform/PlatformSystemProbes.h"

#include <QFile>
#include <QStringList>

#include <algorithm>
#include <cstdio>
#include <limits>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace Spool {
namespace {
    constexpr qint64 kMiB = 1024LL * 1024LL;

    int cpuListSize(const QString& text)
    {
        int count = 0;
        for (const QString& range : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QStringList bounds = range.split(QLatin1Char('-'));
            bool firstOk = false;
            bool lastOk = false;
            const int first = bounds.first().toInt(&firstOk);
            const int last = bounds.size() > 1 ? bounds.last().toInt(&lastOk) : first;
            if (firstOk && (bounds.size() == 1 || lastOk) && last >= first)
                count += last - first + 1;
        }
        return count;
    }

    // Cores the kernel has, not the ones awake right now. A webOS TV parks
    // cores when idle, so `hardware_concurrency()` -- which reports what is
    // online at that instant -- can read 2 on a 4-core set. The probe runs
    // once at startup, so believing it would size the decode pool for the
    // quietest moment of the boot.
    int installedCpuCount()
    {
        QFile present(QStringLiteral("/sys/devices/system/cpu/present"));
        if (!present.open(QIODevice::ReadOnly | QIODevice::Text))
            return 0;
        return cpuListSize(QString::fromUtf8(present.readAll()).trimmed());
    }

    int threadsPerCore()
    {
        QFile siblings(QStringLiteral("/sys/devices/system/cpu/cpu0/topology/thread_siblings_list"));
        if (!siblings.open(QIODevice::ReadOnly | QIODevice::Text))
            return 1;
        return std::max(1, cpuListSize(QString::fromUtf8(siblings.readAll()).trimmed()));
    }

    QByteArray readFile(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return {};
        return file.readAll();
    }

    qint64 memoryValue(const QByteArray& meminfo, const QByteArray& key)
    {
        for (const QByteArray& line : meminfo.split('\n')) {
            if (!line.startsWith(key))
                continue;
            const QList<QByteArray> fields = line.simplified().split(' ');
            bool ok = false;
            const qint64 kib = fields.value(1).toLongLong(&ok);
            if (!ok || kib <= 0 || kib > std::numeric_limits<qint64>::max() / 1024LL)
                return 0;
            return kib * 1024LL;
        }
        return 0;
    }

    qint64 cgroupLimit(const QByteArray& raw)
    {
        const QByteArray value = raw.trimmed();
        if (value.isEmpty() || value == QByteArrayLiteral("max"))
            return 0;
        bool ok = false;
        const qulonglong parsed = value.toULongLong(&ok);
        constexpr qulonglong unlimitedThreshold = 1ULL << 60;
        return ok && parsed > 0 && parsed < unlimitedThreshold ? static_cast<qint64>(parsed) : 0;
    }

    qint64 totalMemoryBytes()
    {
        return effectiveLinuxMemoryBytes(readFile(QStringLiteral("/proc/meminfo")),
            readFile(QStringLiteral("/sys/fs/cgroup/memory.max")),
            readFile(QStringLiteral("/sys/fs/cgroup/memory/memory.limit_in_bytes")));
    }
}

qint64 effectiveLinuxMemoryBytes(
    const QByteArray& meminfo, const QByteArray& cgroupV2Limit, const QByteArray& cgroupV1Limit)
{
    qint64 effective = memoryValue(meminfo, QByteArrayLiteral("MemAvailable:"));
    if (effective <= 0)
        effective = memoryValue(meminfo, QByteArrayLiteral("MemTotal:"));
    for (const qint64 limit : { cgroupLimit(cgroupV2Limit), cgroupLimit(cgroupV1Limit) }) {
        if (limit > 0)
            effective = effective > 0 ? std::min(effective, limit) : limit;
    }
    return effective;
}

PlatformCpuProbe platformCpuProbe(int logicalCpus)
{
    const int siblings = threadsPerCore();
    const int installed = std::max(logicalCpus, installedCpuCount());
    return { std::max(1, (installed + siblings - 1) / siblings), installed,
        siblings > 1 ? QStringLiteral("cpu_present+thread_siblings") : QStringLiteral("cpu_present") };
}

PlatformMemoryPolicy platformMemoryPolicy()
{
    return { totalMemoryBytes(), 1024LL * kMiB, 256LL * kMiB, 256LL * kMiB, 32, 24LL * kMiB, 96LL * kMiB };
}

QString platformProcessMemoryDiagnostics()
{
    long vmrss = 0;
    long rssAnon = 0;
    long vmdata = 0;
    long vmswap = 0;
    if (FILE *file = fopen("/proc/self/status", "r")) {
        char line[256];
        while (fgets(line, sizeof(line), file)) {
            long value = 0;
            if (sscanf(line, "VmRSS: %ld kB", &value) == 1)
                vmrss = value;
            else if (sscanf(line, "RssAnon: %ld kB", &value) == 1)
                rssAnon = value;
            else if (sscanf(line, "VmData: %ld kB", &value) == 1)
                vmdata = value;
            else if (sscanf(line, "VmSwap: %ld kB", &value) == 1)
                vmswap = value;
        }
        fclose(file);
    }

    long long inUse = 0;
    long long free = 0;
    long long arena = 0;
    long long mmap = 0;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    const struct mallinfo2 info = mallinfo2();
    inUse = info.uordblks;
    free = info.fordblks;
    arena = info.arena;
    mmap = info.hblkhd;
#elif defined(__GLIBC__)
    const struct mallinfo info = mallinfo();
    inUse = info.uordblks;
    free = info.fordblks;
    arena = info.arena;
    mmap = info.hblkhd;
#endif
    constexpr long long bytesPerMiB = 1024LL * 1024LL;
    return QStringLiteral("rss=%1M anon=%2M vmdata=%3M swap=%4M | malloc_inuse=%5M arena_free=%6M arena=%7M mmap=%8M")
        .arg(vmrss / 1024)
        .arg(rssAnon / 1024)
        .arg(vmdata / 1024)
        .arg(vmswap / 1024)
        .arg(inUse / bytesPerMiB)
        .arg(free / bytesPerMiB)
        .arg(arena / bytesPerMiB)
        .arg(mmap / bytesPerMiB);
}

} // namespace Spool
