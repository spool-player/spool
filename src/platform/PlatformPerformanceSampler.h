#pragma once

#include <QtGlobal>

#include <memory>

namespace Spool {

struct PlatformPerformanceSample {
    // Whether this process's own CPU time could be read. On Android that is
    // all an ordinary app gets: /proc/self is its own, but the system-wide
    // files are denied to untrusted_app -- silently, because the policy
    // dontaudits most of them. Gating everything on the system figures threw
    // away the per-process and per-thread numbers, which are the ones that
    // actually diagnose playback.
    bool available = false;
    bool systemStatsAvailable = false;
    bool threadBreakdownAvailable = false;
    bool preciseThreadCpuAvailable = false;
    double processCpuPercent = 0.0;
    double systemCpuPercent = 0.0;
    double mpvCpuPercent = 0.0;
    double videoDecodeCpuPercent = 0.0;
    double audioDecodeCpuPercent = 0.0;
    double audioOutputCpuPercent = 0.0;
    double loadOne = 0.0;
    double loadFive = 0.0;
    double loadFifteen = 0.0;
    qint64 processRssBytes = 0;
    qint64 processAnonymousBytes = 0;
    qint64 systemUsedBytes = 0;
    qint64 systemAvailableBytes = 0;
    qint64 systemTotalBytes = 0;
};

class PlatformPerformanceSampler final {
public:
    PlatformPerformanceSampler();
    ~PlatformPerformanceSampler();

    PlatformPerformanceSampler(const PlatformPerformanceSampler&) = delete;
    PlatformPerformanceSampler& operator=(const PlatformPerformanceSampler&) = delete;

    bool sample(qint64 audioDecodeCpuTimeNs, PlatformPerformanceSample& output);

private:
    struct PlatformData;
    std::unique_ptr<PlatformData> m_platform;
};

} // namespace Spool
