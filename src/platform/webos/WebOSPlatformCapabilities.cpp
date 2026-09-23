#include "platform/PlatformCapabilities.h"

namespace Spool {

const PlatformCapabilities& platformCapabilities()
{
    static const PlatformCapabilities capabilities {
        .deviceName = QStringLiteral("LG webOS TV"),
        .rendererName = QStringLiteral("Starfish"),
        .isTV = true,
        .isWebOS = true,
        .hasSystemFonts = false,
        .hasDesktopPointer = false,
        .supportsMpvConfig = false,
        .usesPerOutputAudioDelay = true,
    };
    return capabilities;
}

} // namespace Spool
