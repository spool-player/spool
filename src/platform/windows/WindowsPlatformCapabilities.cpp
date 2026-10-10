#include "platform/PlatformCapabilities.h"

namespace Spool {

const PlatformCapabilities& platformCapabilities()
{
    static const PlatformCapabilities capabilities {
        .deviceName = QStringLiteral("Windows Desktop"),
        .rendererName = QStringLiteral("libmpv OpenGL"),
    };
    return capabilities;
}

} // namespace Spool
