#include "platform/ScreenSaverInhibitor.h"

#include <QDebug>

#include <IOKit/pwr_mgt/IOPMLib.h>

namespace Spool {
namespace {

    class MacOSScreenSaverBackend final : public ScreenSaverBackend {
    public:
        bool acquire() override
        {
            const IOReturn result = IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep,
                kIOPMAssertionLevelOn, CFSTR("Spool media playback"), &m_assertion);
            if (result == kIOReturnSuccess)
                return true;
            qWarning() << "screensaver: macOS idle assertion failed" << result;
            return false;
        }

        bool release() override
        {
            if (m_assertion == kIOPMNullAssertionID)
                return true;
            const IOReturn result = IOPMAssertionRelease(m_assertion);
            if (result != kIOReturnSuccess) {
                qWarning() << "screensaver: macOS idle assertion release failed" << result;
                return false;
            }
            m_assertion = kIOPMNullAssertionID;
            return true;
        }

    private:
        IOPMAssertionID m_assertion = kIOPMNullAssertionID;
    };

} // namespace

std::unique_ptr<ScreenSaverBackend> createPlatformScreenSaverBackend()
{
    return std::make_unique<MacOSScreenSaverBackend>();
}

} // namespace Spool
