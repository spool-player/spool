#include "platform/PlatformProcess.h"

#include <QCoreApplication>

namespace Spool {

ProcessStartupTiming captureProcessStartupTiming()
{
    return {};
}

struct TerminationSignalHandler::PlatformData { };

TerminationSignalHandler::TerminationSignalHandler(QCoreApplication&)
    : m_platform(std::make_unique<PlatformData>())
{
}

TerminationSignalHandler::~TerminationSignalHandler() = default;

} // namespace Spool
