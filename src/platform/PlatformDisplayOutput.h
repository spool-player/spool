#pragma once

class QQuickWindow;

namespace JellyfinNative {

struct DisplayOutputCapabilities;

namespace PlatformDisplayOutput {

    // Render-thread only. Reads Qt's selected format; Wayland PASS_THROUGH
    // ownership is inferred from Qt's selection rule, not queried from the swapchain.
    DisplayOutputCapabilities probe(QQuickWindow *window);

    // GUI-thread only. Enriches the snapshot with compositor-reported luminance.
    void updateDisplayLuminance(DisplayOutputCapabilities& display, QQuickWindow *window);

} // namespace PlatformDisplayOutput

} // namespace JellyfinNative
