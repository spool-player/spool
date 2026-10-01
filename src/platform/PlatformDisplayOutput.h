#pragma once

class QQuickWindow;
class QScreen;

namespace Spool {

struct DisplayOutputCapabilities;

namespace PlatformDisplayOutput {

    // GUI-thread only; unknown state is SDR. This is queried before creating
    // a swapchain, independently of the GPU's supported surface formats.
    bool desktopHdrEnabled(QScreen *screen);

    // Render-thread only. Reads Qt's selected format; Wayland PASS_THROUGH
    // ownership is inferred from Qt's selection rule, not queried from the swapchain.
    DisplayOutputCapabilities probe(QQuickWindow *window);

    // GUI-thread only. Enriches the snapshot with compositor-reported luminance.
    void updateDisplayLuminance(DisplayOutputCapabilities& display, QQuickWindow *window);

} // namespace PlatformDisplayOutput

} // namespace Spool
