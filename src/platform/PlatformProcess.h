#pragma once

#include <QtGlobal>

#include <memory>

class QCoreApplication;

namespace Spool {

struct ProcessStartupTiming {
    qint64 execToMainMs = -1;
    double staticInitializationMs = -1.0;
};

ProcessStartupTiming captureProcessStartupTiming();

class TerminationSignalHandler final {
public:
    explicit TerminationSignalHandler(QCoreApplication& application);
    ~TerminationSignalHandler();

    TerminationSignalHandler(const TerminationSignalHandler&) = delete;
    TerminationSignalHandler& operator=(const TerminationSignalHandler&) = delete;

private:
    struct PlatformData;
    std::unique_ptr<PlatformData> m_platform;
};

} // namespace Spool
