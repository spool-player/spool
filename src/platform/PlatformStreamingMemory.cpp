#include "PlatformSystemProbes.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>

#include <algorithm>
#include <limits>

#if defined(Q_OS_WIN)
#include <windows.h>
#elif defined(Q_OS_DARWIN)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#else
#include <mach/mach.h>
#endif
#endif

namespace Spool {
namespace {
#if defined(Q_OS_LINUX)
    QByteArray read(const QString& path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    QString mountPath(QByteArray value)
    {
        value.replace("\\040", " ");
        value.replace("\\011", "\t");
        value.replace("\\134", "\\");
        return QString::fromLocal8Bit(value);
    }

    void applyCgroupLimit(qint64& available)
    {
        const auto membership = read(QStringLiteral("/proc/self/cgroup")).split('\n');
        const auto mounts = read(QStringLiteral("/proc/self/mountinfo")).split('\n');
        for (const auto& member : membership) {
            const auto fields = member.split(':');
            if (fields.size() != 3)
                continue;
            const bool unified = fields[0] == "0" && fields[1].isEmpty();
            if (!unified && !fields[1].split(',').contains("memory"))
                continue;
            for (const auto& mount : mounts) {
                const auto halves = mount.split(' ');
                const int separator = halves.indexOf("-");
                if (separator < 0 || halves.size() <= separator + 3 || halves.size() < 6)
                    continue;
                const bool match = unified
                    ? halves[separator + 1] == "cgroup2"
                    : halves[separator + 1] == "cgroup" && halves[separator + 3].split(',').contains("memory");
                if (!match)
                    continue;
                const QString root = mountPath(halves[3]);
                const QString mountpoint = mountPath(halves[4]);
                const QString group = QString::fromLocal8Bit(fields[2]);
                if (root != QLatin1String("/") && group != root && !group.startsWith(root + QLatin1Char('/')))
                    continue;
                QString suffix = root == QLatin1String("/") ? group : group.mid(root.size());
                QString directory = QDir::cleanPath(mountpoint + QLatin1Char('/') + suffix);
                // Every ancestor may impose a tighter limit than the leaf.
                while (directory == mountpoint || directory.startsWith(mountpoint + QLatin1Char('/'))) {
                    bool limitOk = false, currentOk = false;
                    const qint64 limit = read(directory
                        + (unified ? QStringLiteral("/memory.max") : QStringLiteral("/memory.limit_in_bytes")))
                                             .trimmed()
                                             .toLongLong(&limitOk);
                    const qint64 current = read(directory
                        + (unified ? QStringLiteral("/memory.current") : QStringLiteral("/memory.usage_in_bytes")))
                                               .trimmed()
                                               .toLongLong(&currentOk);
                    if (limitOk && currentOk && limit > 0 && current >= 0
                        && limit < std::numeric_limits<qint64>::max() / 2)
                        available = std::min(available, std::max(qint64(0), limit - current));
                    if (directory == mountpoint)
                        break;
                    directory = QFileInfo(directory).dir().absolutePath();
                }
            }
        }
    }
#endif
}

PlatformStreamingMemoryState platformStreamingMemoryState()
{
#if defined(Q_OS_LINUX)
    for (const auto& line : read(QStringLiteral("/proc/meminfo")).split('\n')) {
        if (!line.startsWith("MemAvailable:"))
            continue;
        bool ok = false;
        const auto fields = line.simplified().split(' ');
        const qint64 kib = fields.size() >= 2 ? fields[1].toLongLong(&ok) : 0;
        if (!ok || kib < 0 || kib > std::numeric_limits<qint64>::max() / 1024)
            return {};
        qint64 available = kib * 1024;
        applyCgroupLimit(available);
        return { available, true };
    }
#elif defined(Q_OS_WIN)
    MEMORYSTATUSEX state {};
    state.dwLength = sizeof(state);
    if (GlobalMemoryStatusEx(&state))
        return { qint64(std::min<quint64>(state.ullAvailPhys, std::numeric_limits<qint64>::max())), true };
#elif defined(Q_OS_DARWIN)
#if TARGET_OS_IPHONE
    if (__builtin_available(iOS 13.0, tvOS 13.0, *))
        return { qint64(os_proc_available_memory()), true };
#else
    vm_statistics64_data_t state {};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t pageSize = 0;
    const mach_port_t host = mach_host_self();
    const bool success = host_page_size(host, &pageSize) == KERN_SUCCESS
        && host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&state), &count) == KERN_SUCCESS;
    mach_port_deallocate(mach_task_self(), host);
    if (success)
        return { qint64(state.free_count + state.inactive_count) * qint64(pageSize), true };
#endif
#endif
    return {};
}
} // namespace Spool
