#include "platform/PlatformProcess.h"

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
