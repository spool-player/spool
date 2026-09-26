#include "platform/ScreenSaverInhibitor.h"

#include <QDebug>

#include <windows.h>

namespace Spool {
namespace {

    class WindowsScreenSaverBackend final : public ScreenSaverBackend {
    public:
        bool acquire() override
        {
            return apply(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
        }

        bool release() override
        {
            return apply(ES_CONTINUOUS);
        }

    private:
        static bool apply(EXECUTION_STATE state)
        {
            if (SetThreadExecutionState(state) != 0)
                return true;
            qWarning() << "screensaver: SetThreadExecutionState failed";
            return false;
        }
    };

} // namespace

std::unique_ptr<ScreenSaverBackend> createPlatformScreenSaverBackend()
{
    return std::make_unique<WindowsScreenSaverBackend>();
}

} // namespace Spool
