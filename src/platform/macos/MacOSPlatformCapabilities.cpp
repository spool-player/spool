#include "platform/PlatformCapabilities.h"

namespace Spool {

const PlatformCapabilities& platformCapabilities()
{
    static const PlatformCapabilities capabilities {
        .deviceName = QStringLiteral("macOS Desktop"),
        .rendererName = QStringLiteral("libmpv OpenGL"),
    };
    return capabilities;
}

} // namespace Spool
