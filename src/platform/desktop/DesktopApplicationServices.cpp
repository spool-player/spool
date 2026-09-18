#include "platform/PlatformApplicationServices.h"

namespace JellyfinNative {

struct PlatformApplicationServices::PlatformData { };

PlatformApplicationServices::PlatformApplicationServices(
    QGuiApplication&, NativeAppWindow&, ApplicationHooks&, RouterController&)
    : m_platform(std::make_unique<PlatformData>())
{
}

PlatformApplicationServices::~PlatformApplicationServices() = default;

void PlatformApplicationServices::start() { }

} // namespace JellyfinNative
