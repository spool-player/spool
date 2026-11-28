#include "platform/NativeAppWindow.h"

#include <QGuiApplication>

#import <AppKit/AppKit.h>

namespace Spool {
void NativeAppWindow::configureMacOSTitlebar()
{
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return;
    NSWindow *window = reinterpret_cast<NSView *>(winId()).window;
    if (!window)
        return;
    window.titlebarAppearsTransparent = YES;
}

void NativeAppWindow::setTitlebarVisible(bool visible)
{
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return;
    NSWindow *window = reinterpret_cast<NSView *>(winId()).window;
    if (!window)
        return;
    window.titlebarAppearsTransparent = YES;
    window.titleVisibility = visible ? NSWindowTitleVisible : NSWindowTitleHidden;
    for (NSWindowButton button : { NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton })
        [window standardWindowButton:button].hidden = !visible;
}
} // namespace Spool
