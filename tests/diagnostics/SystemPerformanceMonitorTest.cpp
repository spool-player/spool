#include "diagnostics/SystemPerformanceMonitor.h"

#include "TestMain.h"
#include "TestRequire.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QTimer>

#include <atomic>
#include <cstdlib>
#include <iostream>

namespace {

using SpoolTests::require;

} // namespace

SPOOL_TEST_MAIN("system-performance-monitor")
{
    QCoreApplication app(argc, argv);
    Spool::SystemPerformanceMonitor monitor;
    std::atomic<qint64> fakeAudioDecodeTimeNs { 0 };
    monitor.setAudioDecodeCpuTimeProvider(
        [&fakeAudioDecodeTimeNs] { return fakeAudioDecodeTimeNs.fetch_add(1'000'000) + 1'000'000; });

#ifdef Q_OS_LINUX
    // Counters only become meaningful once a second sampling tick has landed.
    // Platforms without counters have nothing to wait for, so they skip it.
    QEventLoop waitForSecondSample;
    QTimer::singleShot(1100, &waitForSecondSample, &QEventLoop::quit);
    waitForSecondSample.exec();

    require(monitor.available(), "Linux performance counters should be available");
    require(monitor.threadBreakdownAvailable(), "Linux thread counters should be available");
    QFile schedstat(QStringLiteral("/proc/self/task/%1/schedstat").arg(QCoreApplication::applicationPid()));
    if (schedstat.open(QIODevice::ReadOnly))
        require(monitor.preciseThreadCpuAvailable(), "readable schedstat should enable precise thread CPU counters");
    require(monitor.processCpuPercent() >= 0.0, "process CPU should be non-negative");
    require(monitor.audioDecodeCpuPercent() > 0.0, "audio decode provider should populate CPU usage");
    require(
        monitor.systemCpuPercent() >= 0.0 && monitor.systemCpuPercent() <= 100.0, "system CPU should be a percentage");
    require(monitor.processRssBytes() > 0, "process RSS should be populated");
#ifndef Q_OS_ANDROID
    require(monitor.systemStatsAvailable(), "native Linux exposes its global performance counters");
#endif
    // CPU-wide proc access and memory-wide proc access are independent on
    // Android: readable meminfo remains real data when /proc/stat is denied.
    QFile meminfo(QStringLiteral("/proc/meminfo"));
    if (meminfo.open(QIODevice::ReadOnly)) {
        require(monitor.systemTotalBytes() > 0, "readable system memory should be populated");
        require(monitor.systemAvailableBytes() > 0 && monitor.systemAvailableBytes() <= monitor.systemTotalBytes(),
            "readable available memory should stay inside system capacity");
        require(monitor.systemUsedBytes() > 0
                && monitor.systemUsedBytes() + monitor.systemAvailableBytes() == monitor.systemTotalBytes(),
            "real used and available memory account for the system capacity");
    } else {
        require(
            monitor.systemTotalBytes() == 0 && monitor.systemAvailableBytes() == 0 && monitor.systemUsedBytes() == 0,
            "unreadable memory counters remain zero rather than invented");
    }
    if (!monitor.systemStatsAvailable())
        require(monitor.systemCpuPercent() == 0.0, "unavailable global CPU counter remains zero");
#else
    require(!monitor.available(), "unsupported platforms should report unavailable counters");
    require(!monitor.threadBreakdownAvailable() && !monitor.preciseThreadCpuAvailable(),
        "unsupported platforms must not advertise thread counters either");
    require(monitor.processCpuPercent() == 0.0 && monitor.processRssBytes() == 0 && monitor.systemTotalBytes() == 0,
        "unsupported platforms must report zeroed counters rather than stale or invented readings");
#endif
    return 0;
}
