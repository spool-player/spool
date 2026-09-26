#include "platform/PlatformPerformanceSampler.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QStringList>

#include <algorithm>
#include <unistd.h>

namespace Spool {
namespace {
    QByteArray readFile(const QString& path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray {};
    }

    bool parseStat(const QByteArray& contents, QString *name, quint64 *ticks)
    {
        const qsizetype closeParen = contents.lastIndexOf(')');
        const qsizetype openParen = contents.indexOf('(');
        if (openParen < 0 || closeParen <= openParen || closeParen + 2 >= contents.size())
            return false;
        const QList<QByteArray> fields = contents.mid(closeParen + 2).simplified().split(' ');
        if (fields.size() <= 12)
            return false;
        bool userOk = false;
        bool systemOk = false;
        const quint64 userTicks = fields.at(11).toULongLong(&userOk);
        const quint64 systemTicks = fields.at(12).toULongLong(&systemOk);
        if (!userOk || !systemOk)
            return false;
        if (name)
            *name = QString::fromUtf8(contents.mid(openParen + 1, closeParen - openParen - 1));
        *ticks = userTicks + systemTicks;
        return true;
    }

    bool parseSchedstat(const QByteArray& contents, quint64 *runtimeNs)
    {
        bool ok = false;
        const quint64 value = contents.simplified().split(' ').value(0).toULongLong(&ok);
        if (ok)
            *runtimeNs = value;
        return ok;
    }

    bool isMpvThread(const QString& name)
    {
        return name == QStringLiteral("core") || name == QStringLiteral("demux") || name == QStringLiteral("input")
            || name == QStringLiteral("opener") || name == QStringLiteral("vo") || name == QStringLiteral("ao")
            || name == QStringLiteral("worker") || name == QStringLiteral("curl") || name == QStringLiteral("log")
            || name.startsWith(QStringLiteral("dec/")) || name.startsWith(QStringLiteral("ao/"))
            || name.startsWith(QStringLiteral("video-")) || name.startsWith(QStringLiteral("lxvideodec"));
    }

    double percentForTicks(quint64 ticks, double seconds, long ticksPerSecond)
    {
        return seconds > 0.0 && ticksPerSecond > 0
            ? static_cast<double>(ticks) * 100.0 / (seconds * static_cast<double>(ticksPerSecond))
            : 0.0;
    }
} // namespace

struct PlatformPerformanceSampler::PlatformData {
    struct ThreadSample {
        QString name;
        quint64 ticks = 0;
        quint64 runtimeNs = 0;
        bool precise = false;
    };

    QElapsedTimer elapsed;
    QHash<qint64, ThreadSample> previousThreads;
    quint64 previousProcessTicks = 0;
    quint64 previousSystemTotal = 0;
    quint64 previousSystemIdle = 0;
    // Reading a file the kernel refuses costs a syscall and, on Android, an
    // audit line every second for a figure that is never going to arrive.
    // Ask once.
    bool systemFilesReadable = true;
    qint64 previousSampleNs = 0;
    qint64 previousAudioDecodeCpuTimeNs = -1;
    long clockTicksPerSecond = 100;
};

PlatformPerformanceSampler::PlatformPerformanceSampler()
    : m_platform(std::make_unique<PlatformData>())
{
    const long ticks = sysconf(_SC_CLK_TCK);
    if (ticks > 0)
        m_platform->clockTicksPerSecond = ticks;
    m_platform->elapsed.start();
}

PlatformPerformanceSampler::~PlatformPerformanceSampler() = default;

bool PlatformPerformanceSampler::sample(qint64 audioDecodeCpuTimeNs, PlatformPerformanceSample& output)
{
    auto& state = *m_platform;
    const qint64 nowNs = state.elapsed.nsecsElapsed();
    const double elapsedSeconds
        = state.previousSampleNs > 0 ? static_cast<double>(nowNs - state.previousSampleNs) / 1'000'000'000.0 : 0.0;

    quint64 processTicks = 0;
    const bool processOk = parseStat(readFile(QStringLiteral("/proc/self/stat")), nullptr, &processTicks);
    quint64 systemTotal = 0;
    quint64 systemIdle = 0;
    const QList<QByteArray> systemFields
        = readFile(QStringLiteral("/proc/stat")).split('\n').value(0).simplified().split(' ');
    bool systemOk = systemFields.size() >= 5 && systemFields.value(0) == "cpu";
    if (systemOk) {
        for (qsizetype index = 1; index < systemFields.size(); ++index) {
            bool ok = false;
            const quint64 value = systemFields.at(index).toULongLong(&ok);
            systemOk = systemOk && ok;
            systemTotal += value;
            if (index == 4 || index == 5)
                systemIdle += value;
        }
    }

    QHash<qint64, PlatformData::ThreadSample> threads;
    const QStringList tids
        = QDir(QStringLiteral("/proc/self/task")).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    quint64 mpvTicks = 0;
    quint64 videoDecodeTicks = 0;
    quint64 audioDecodeTicks = 0;
    quint64 audioOutputTicks = 0;
    quint64 mpvRuntimeNs = 0;
    quint64 videoDecodeRuntimeNs = 0;
    quint64 audioDecodeRuntimeNs = 0;
    quint64 audioOutputRuntimeNs = 0;
    bool preciseAvailable = false;
    for (const QString& tidText : tids) {
        bool tidOk = false;
        const qint64 tid = tidText.toLongLong(&tidOk);
        PlatformData::ThreadSample current;
        if (!tidOk
            || !parseStat(
                readFile(QStringLiteral("/proc/self/task/%1/stat").arg(tidText)), &current.name, &current.ticks)) {
            continue;
        }
        current.precise
            = parseSchedstat(readFile(QStringLiteral("/proc/self/task/%1/schedstat").arg(tidText)), &current.runtimeNs);
        preciseAvailable = preciseAvailable || current.precise;
        threads.insert(tid, current);
        const auto previous = state.previousThreads.constFind(tid);
        if (previous == state.previousThreads.cend() || previous->name != current.name
            || current.ticks < previous->ticks)
            continue;
        const bool precise = current.precise && previous->precise && current.runtimeNs >= previous->runtimeNs;
        const quint64 delta = precise ? current.runtimeNs - previous->runtimeNs : current.ticks - previous->ticks;
        quint64& mpvTotal = precise ? mpvRuntimeNs : mpvTicks;
        quint64& videoTotal = precise ? videoDecodeRuntimeNs : videoDecodeTicks;
        quint64& audioTotal = precise ? audioDecodeRuntimeNs : audioDecodeTicks;
        quint64& outputTotal = precise ? audioOutputRuntimeNs : audioOutputTicks;
        if (isMpvThread(current.name))
            mpvTotal += delta;
        if (current.name == QStringLiteral("dec/video") || current.name.startsWith(QStringLiteral("lxvideodec")))
            videoTotal += delta;
        else if (current.name == QStringLiteral("dec/audio"))
            audioTotal += delta;
        else if (current.name == QStringLiteral("ao") || current.name.startsWith(QStringLiteral("ao/")))
            outputTotal += delta;
    }

    if (state.systemFilesReadable) {
        const QList<QByteArray> loadFields = readFile(QStringLiteral("/proc/loadavg")).simplified().split(' ');
        if (loadFields.size() >= 3) {
            output.loadOne = loadFields.at(0).toDouble();
            output.loadFive = loadFields.at(1).toDouble();
            output.loadFifteen = loadFields.at(2).toDouble();
        } else {
            state.systemFilesReadable = false;
        }
    }
    for (const QByteArray& line : readFile(QStringLiteral("/proc/self/status")).split('\n')) {
        const QList<QByteArray> fields = line.simplified().split(' ');
        if (fields.size() < 2)
            continue;
        if (fields.at(0) == "VmRSS:")
            output.processRssBytes = fields.at(1).toLongLong() * 1024;
        else if (fields.at(0) == "RssAnon:")
            output.processAnonymousBytes = fields.at(1).toLongLong() * 1024;
    }
    for (const QByteArray& line : readFile(QStringLiteral("/proc/meminfo")).split('\n')) {
        const QList<QByteArray> fields = line.simplified().split(' ');
        if (fields.size() < 2)
            continue;
        if (fields.at(0) == "MemTotal:")
            output.systemTotalBytes = fields.at(1).toLongLong() * 1024;
        else if (fields.at(0) == "MemAvailable:")
            output.systemAvailableBytes = fields.at(1).toLongLong() * 1024;
    }
    output.systemUsedBytes = std::max<qint64>(0, output.systemTotalBytes - output.systemAvailableBytes);

    if (elapsedSeconds > 0.0) {
        if (processOk && processTicks >= state.previousProcessTicks) {
            output.processCpuPercent
                = percentForTicks(processTicks - state.previousProcessTicks, elapsedSeconds, state.clockTicksPerSecond);
        }
        if (systemOk && systemTotal >= state.previousSystemTotal && systemIdle >= state.previousSystemIdle) {
            const quint64 totalDelta = systemTotal - state.previousSystemTotal;
            const quint64 idleDelta = systemIdle - state.previousSystemIdle;
            if (totalDelta > 0) {
                output.systemCpuPercent = 100.0 * static_cast<double>(totalDelta - std::min(totalDelta, idleDelta))
                    / static_cast<double>(totalDelta);
            }
        }
        const double elapsedNs = elapsedSeconds * 1'000'000'000.0;
        const auto precisePercent = [elapsedNs](quint64 runtimeNs) {
            return elapsedNs > 0.0 ? static_cast<double>(runtimeNs) * 100.0 / elapsedNs : 0.0;
        };
        output.mpvCpuPercent
            = precisePercent(mpvRuntimeNs) + percentForTicks(mpvTicks, elapsedSeconds, state.clockTicksPerSecond);
        output.videoDecodeCpuPercent = precisePercent(videoDecodeRuntimeNs)
            + percentForTicks(videoDecodeTicks, elapsedSeconds, state.clockTicksPerSecond);
        if (audioDecodeCpuTimeNs >= 0 && state.previousAudioDecodeCpuTimeNs >= 0
            && audioDecodeCpuTimeNs >= state.previousAudioDecodeCpuTimeNs) {
            output.audioDecodeCpuPercent
                = precisePercent(static_cast<quint64>(audioDecodeCpuTimeNs - state.previousAudioDecodeCpuTimeNs));
        } else {
            output.audioDecodeCpuPercent = precisePercent(audioDecodeRuntimeNs)
                + percentForTicks(audioDecodeTicks, elapsedSeconds, state.clockTicksPerSecond);
        }
        output.audioOutputCpuPercent = precisePercent(audioOutputRuntimeNs)
            + percentForTicks(audioOutputTicks, elapsedSeconds, state.clockTicksPerSecond);
    }

    output.available = processOk;
    output.systemStatsAvailable = systemOk && output.systemTotalBytes > 0;
    output.threadBreakdownAvailable = !threads.isEmpty();
    output.preciseThreadCpuAvailable = preciseAvailable;
    state.previousProcessTicks = processTicks;
    state.previousSystemTotal = systemTotal;
    state.previousSystemIdle = systemIdle;
    state.previousThreads = std::move(threads);
    state.previousSampleNs = nowNs;
    state.previousAudioDecodeCpuTimeNs = audioDecodeCpuTimeNs;
    return true;
}

} // namespace Spool
