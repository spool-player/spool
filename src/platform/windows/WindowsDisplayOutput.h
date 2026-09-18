#pragma once

class QRhiSwapChain;

namespace JellyfinNative {

struct DisplayOutputCapabilities;

// Render-thread only, for a Qt D3D11 swapchain. Establishes the DXGI color
// space matching its actual buffer before reporting the embedding contract.
DisplayOutputCapabilities windowsD3D11DisplayOutput(QRhiSwapChain *swapchain);

} // namespace JellyfinNative
