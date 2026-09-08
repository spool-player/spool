#include "TestMain.h"
#include "platform/NativeAppWindow.h"

#include <QGuiApplication>

#import <AppKit/AppKit.h>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
}

JELLYFIN_TEST_MAIN("macos-titlebar")
{
    qputenv("QT_QPA_PLATFORM", "cocoa");
    QGuiApplication app(argc, argv);
    JellyfinNative::NativeAppWindow window(QStringLiteral("titlebar-test"));
    window.show();
    app.processEvents();
    NSWindow *native = reinterpret_cast<NSView *>(window.winId()).window;
    const NSRect frame = native.frame;
    const NSRect content = native.contentView.frame;
    require(content.size.height == frame.size.height, "content extends beneath the native titlebar");
    require(native.titlebarAppearsTransparent, "titlebar background does not obscure content");
    for (bool visible : { false, true, false, true }) {
        window.setTitlebarVisible(visible);
        app.processEvents();
        require((native.titleVisibility == NSWindowTitleVisible) == visible, "title follows playback chrome");
        for (NSWindowButton button : { NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton })
            require([native standardWindowButton:button].hidden == !visible, "window buttons follow playback chrome");
        require(NSEqualRects(native.frame, frame) && NSEqualRects(native.contentView.frame, content),
            "hiding chrome does not resize the window or video");
    }
    return 0;
}
