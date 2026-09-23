#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace Spool {

struct PlatformCpuProbe {
    int physicalCores = 0;
    // CPUs the kernel knows about, including any currently parked. Zero means
    // the platform cannot tell, in which case the online count stands.
    int installedCpus = 0;
    QString source;
};

struct PlatformMemoryPolicy {
    qint64 totalBytes = 0;
    qint64 fallbackBytes = 0;
    qint64 baseNetworkDiskCacheBytes = 0;
    qint64 baseQmlImageDiskCacheBytes = 0;
    int artworkDivisor = 32;
    qint64 minimumArtworkBytes = 0;
    qint64 maximumArtworkBytes = 0;
};

PlatformCpuProbe platformCpuProbe(int logicalCpus);
PlatformMemoryPolicy platformMemoryPolicy();
qint64 effectiveLinuxMemoryBytes(
    const QByteArray& meminfo, const QByteArray& cgroupV2Limit, const QByteArray& cgroupV1Limit);
QString platformProcessMemoryDiagnostics();

} // namespace Spool
