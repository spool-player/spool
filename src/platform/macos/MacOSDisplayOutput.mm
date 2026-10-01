#include "MacOSDisplayOutput.h"

#import <AppKit/AppKit.h>
#include <QScreen>

#include <cmath>

namespace Spool {
bool macosDesktopHdrEnabled(QScreen *screen)
{
    if (!screen)
        return false;
    const auto *native = screen->nativeInterface<QNativeInterface::QCocoaScreen>();
    NSScreen *output = native ? native->nativeScreen() : nil;
    if (!output)
        return false;
    if (@available(macOS 10.15, *)) {
        // Current headroom, not maximumPotentialExtendedDynamicRangeColorComponentValue:
        // an HDR-capable panel running in SDR must not enable our HDR surface.
        const double headroom = output.maximumExtendedDynamicRangeColorComponentValue;
        return std::isfinite(headroom) && headroom > 1.0;
    }
    return false;
}
}
