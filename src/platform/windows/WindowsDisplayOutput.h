#pragma once

class QRhiSwapChain;
class QScreen;

namespace Spool {

struct DisplayOutputCapabilities;

// Reads Windows' active desktop color space before any swapchain exists.
bool windowsDesktopHdrEnabled(QScreen *screen);

// Render-thread only, for a Qt D3D11 swapchain. Establishes the DXGI color
// space matching its actual buffer before reporting the embedding contract.
DisplayOutputCapabilities windowsD3D11DisplayOutput(QRhiSwapChain *swapchain);

} // namespace Spool
