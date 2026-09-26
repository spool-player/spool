#pragma once

#include <QString>

namespace Spool {

struct PlatformCapabilities {
    QString deviceName;
    QString rendererName;
    bool isTV = false;
    bool isWebOS = false;
    bool isAndroid = false;
    bool isMobile = false;
    bool hasSystemFonts = true;
    bool hasDesktopPointer = true;
    // Whether anything on this device can point at and drag a control. A
    // television is not automatically a no: webOS's magic remote is a pointer,
    // an Android TV remote is a d-pad and nothing else.
    bool hasPointer = true;
    bool supportsMpvConfig = true;
    bool usesPerOutputAudioDelay = false;
};

const PlatformCapabilities& platformCapabilities();

} // namespace Spool
