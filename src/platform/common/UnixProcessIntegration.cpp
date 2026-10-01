#include "platform/PlatformProcess.h"

#include "diagnostics/Diagnostics.h"

#include <QCoreApplication>
#include <QFile>
#include <QList>
#include <QSocketNotifier>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <ctime>

namespace Spool {

namespace {
    qint64 g_staticInitializationNs = 0;
    volatile sig_atomic_t g_signalWriteFd = -1;

    __attribute__((constructor)) void recordStaticInitializationStart()
    {
        timespec value {};
        if (clock_gettime(CLOCK_MONOTONIC, &value) == 0)
            g_staticInitializationNs = static_cast<qint64>(value.tv_sec) * 1000000000LL + value.tv_nsec;
    }

    void handleSignal(int signalNumber)
    {
        const int fd = static_cast<int>(g_signalWriteFd);
        if (fd < 0)
            return;
        const char byte = static_cast<char>(signalNumber > 0 ? signalNumber : 1);
        const ssize_t ignored = write(fd, &byte, 1);
        (void)ignored;
    }
} // namespace

ProcessStartupTiming captureProcessStartupTiming()
{
    ProcessStartupTiming timing;
    timespec mainTimestamp {};
    if (clock_gettime(CLOCK_MONOTONIC, &mainTimestamp) == 0 && g_staticInitializationNs > 0) {
        const qint64 mainNs = static_cast<qint64>(mainTimestamp.tv_sec) * 1000000000LL + mainTimestamp.tv_nsec;
        timing.staticInitializationMs = static_cast<double>(mainNs - g_staticInitializationNs) / 1000000.0;
    }

    QFile statFile(QStringLiteral("/proc/self/stat"));
    QFile uptimeFile(QStringLiteral("/proc/uptime"));
    if (statFile.open(QIODevice::ReadOnly) && uptimeFile.open(QIODevice::ReadOnly)) {
        const QByteArray stat = statFile.readAll();
        const qsizetype commEnd = stat.lastIndexOf(')');
        const QList<QByteArray> fields = commEnd >= 0 ? stat.mid(commEnd + 2).split(' ') : QList<QByteArray> {};
        bool startOk = false;
        bool uptimeOk = false;
        const qulonglong startTicks = fields.value(19).toULongLong(&startOk);
        const double uptimeSeconds = uptimeFile.readAll().split(' ').value(0).toDouble(&uptimeOk);
        const long ticksPerSecond = sysconf(_SC_CLK_TCK);
        if (startOk && uptimeOk && ticksPerSecond > 0) {
            timing.execToMainMs = qRound64((uptimeSeconds - static_cast<double>(startTicks) / ticksPerSecond) * 1000.0);
        }
    }
    return timing;
}

struct TerminationSignalHandler::PlatformData {
    int readFd = -1;
    int writeFd = -1;
    std::unique_ptr<QSocketNotifier> notifier;

    ~PlatformData()
    {
        notifier.reset();
        if (g_signalWriteFd == writeFd)
            g_signalWriteFd = -1;
        struct sigaction action {};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        sigaction(SIGINT, &action, nullptr);
        sigaction(SIGTERM, &action, nullptr);
        if (readFd >= 0)
            close(readFd);
        if (writeFd >= 0)
            close(writeFd);
    }
};

TerminationSignalHandler::TerminationSignalHandler(QCoreApplication& application)
    : m_platform(std::make_unique<PlatformData>())
{
    int descriptors[2] { -1, -1 };
    if (pipe(descriptors) != 0)
        return;
    m_platform->readFd = descriptors[0];
    m_platform->writeFd = descriptors[1];
    for (int fd : descriptors) {
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    g_signalWriteFd = m_platform->writeFd;
    struct sigaction action {};
    action.sa_handler = handleSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    m_platform->notifier = std::make_unique<QSocketNotifier>(m_platform->readFd, QSocketNotifier::Read);
    QObject::connect(m_platform->notifier.get(), &QSocketNotifier::activated, &application,
        [&application, platform = m_platform.get()](QSocketDescriptor, QSocketNotifier::Type) {
            char buffer[16];
            ssize_t count = 0;
            while ((count = read(platform->readFd, buffer, sizeof(buffer))) > 0) {
                for (ssize_t index = 0; index < count; ++index) {
                    Diagnostics::logEvent(QStringLiteral("signal"), QStringLiteral("received"),
                        { { QStringLiteral("signal"), static_cast<int>(buffer[index]) } });
                }
            }
            application.quit();
        });
}

TerminationSignalHandler::~TerminationSignalHandler() = default;

} // namespace Spool
