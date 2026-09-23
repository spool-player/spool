#include "platform/PlatformPerformanceSampler.h"

namespace Spool {

struct PlatformPerformanceSampler::PlatformData { };

PlatformPerformanceSampler::PlatformPerformanceSampler()
    : m_platform(std::make_unique<PlatformData>())
{
}

PlatformPerformanceSampler::~PlatformPerformanceSampler() = default;

bool PlatformPerformanceSampler::sample(qint64, PlatformPerformanceSample&)
{
    return false;
}

} // namespace Spool
