#include "platform/PlatformSystemProbes.h"

#include "TestMain.h"

#include <QtGlobal>

#include <cstdlib>
#include <iostream>
#include <limits>

using namespace Spool;

namespace {
void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}
}

SPOOL_TEST_MAIN("linux-memory-policy")
{
#ifdef Q_OS_LINUX
    constexpr qint64 mib = 1024LL * 1024LL;
    require(effectiveLinuxMemoryBytes("MemTotal: 8192000 kB\nMemAvailable: 4096000 kB\n", {}, {}) == 4000 * mib,
        "MemAvailable should drive the effective budget");
    require(effectiveLinuxMemoryBytes("MemTotal: 8192000 kB\nMemAvailable: malformed kB\n", {}, {}) == 8000 * mib,
        "malformed MemAvailable should fall back to MemTotal");
    require(effectiveLinuxMemoryBytes("MemTotal: 2048000 kB\nMemAvailable: 0 kB\n", {}, {}) == 2000 * mib,
        "zero MemAvailable should fall back to MemTotal");
    require(effectiveLinuxMemoryBytes("MemTotal: 1048576 kB\n", {}, {}) == 1024 * mib,
        "missing MemAvailable should fall back to MemTotal");
    require(effectiveLinuxMemoryBytes("MemAvailable: 90000000000000000 kB\n", {}, {}) == 0,
        "overflowing memory values should be rejected");
    require(effectiveLinuxMemoryBytes("MemAvailable: 4096000 kB\n", QByteArray::number(768 * mib), {}) == 768 * mib,
        "cgroup v2 should cap host memory");
    require(effectiveLinuxMemoryBytes("MemAvailable: 4096000 kB\n", "max", QByteArray::number(512 * mib)) == 512 * mib,
        "cgroup v1 should apply when v2 is unlimited");
    require(effectiveLinuxMemoryBytes({}, {}, {}) == 0, "missing probes should request the conservative fallback");
#endif
    return EXIT_SUCCESS;
}
