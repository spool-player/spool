import QtQuick

// The first frame predates the singletons. main.cpp exposes the same native
// window through the startup context until the shell takes over.
HdrUiLayer {
    width: 1920
    height: 1080
    hdrOutput: typeof startupNativeWindow !== "undefined" && startupNativeWindow ? startupNativeWindow.hdrOutput : false
    hdrSdrWhiteNits: typeof startupNativeWindow !== "undefined" && startupNativeWindow
                     ? startupNativeWindow.hdrSdrWhiteNits : 203

    SplashContent {
        anchors.fill: parent
        pixelsPerDp: startupSplashPixelsPerDp
        coreWidthDp: startupSplashCoreWidthDp
        coreWidthFraction: startupSplashCoreWidthFraction
        coreAspect: startupSplashCoreAspect
        coreSource: startupSplashImageUrl
    }
}
