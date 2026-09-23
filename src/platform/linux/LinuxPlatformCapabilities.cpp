#include "platform/PlatformCapabilities.h"

namespace Spool {

const PlatformCapabilities& platformCapabilities()
{
    static const PlatformCapabilities capabilities {
        .deviceName = QStringLiteral("Linux Desktop"),
        .rendererName = QStringLiteral("libmpv OpenGL"),
    };
    return capabilities;
}

} // namespace Spool
