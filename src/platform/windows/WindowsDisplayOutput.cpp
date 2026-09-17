#include "platform/windows/WindowsDisplayOutput.h"

#include "player/RenderTargetProfile.h"

#include <QDebug>
#include <QWindow>

#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

// QD3D11SwapChain is how the DXGI swapchain behind QRhiSwapChain is reached.
// An installed Qt keeps the backend headers under private/, not rhi/.
#include <private/qrhid3d11_p.h>

#include <cmath>
#include <cwchar>
#include <optional>
#include <vector>

namespace JellyfinNative {
namespace {

    using Microsoft::WRL::ComPtr;

    std::optional<float> sdrWhiteNits(HMONITOR monitor)
    {
        MONITORINFOEXW monitorInfo {};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (!GetMonitorInfoW(monitor, &monitorInfo))
            return {};

        // Display topology can change between sizing and querying. Bound the
        // retry; a missing startup value uses the shared reference-white fallback.
        for (int attempt = 0; attempt < 3; ++attempt) {
            UINT32 pathCount = 0;
            UINT32 modeCount = 0;
            if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
                return {};
            std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
            std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
            const LONG result = QueryDisplayConfig(
                QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
            if (result == ERROR_INSUFFICIENT_BUFFER)
                continue;
            if (result != ERROR_SUCCESS)
                return {};

            for (UINT32 i = 0; i < pathCount; ++i) {
                const auto& path = paths[i];
                DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
                source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                source.header.size = sizeof(source);
                source.header.adapterId = path.sourceInfo.adapterId;
                source.header.id = path.sourceInfo.id;
                if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS
                    || std::wcscmp(monitorInfo.szDevice, source.viewGdiDeviceName) != 0)
                    continue;

                DISPLAYCONFIG_SDR_WHITE_LEVEL white {};
                white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
                white.header.size = sizeof(white);
                white.header.adapterId = path.targetInfo.adapterId;
                white.header.id = path.targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0)
                    return float(white.SDRWhiteLevel) * 80.0f / 1000.0f;
            }
            return {};
        }
        return {};
    }

} // namespace

DisplayOutputCapabilities windowsD3D11DisplayOutput(QRhiSwapChain *swapchain)
{
    DisplayOutputCapabilities display;
    // Qt exposes no DXGI swapchain through QRhi's native handles. Keep this
    // private-header dependency confined here and recheck it when updating Qt.
    // QRhiSwapChain::format() is a request: Qt can fall back without changing it.
    auto *native = static_cast<QD3D11SwapChain *>(swapchain)->swapChain;
    if (!native)
        return display;

    DXGI_SWAP_CHAIN_DESC desc {};
    const HRESULT descResult = native->GetDesc(&desc);
    if (FAILED(descResult)) {
        qWarning("output: D3D11 cannot inspect presentation buffer (HRESULT 0x%08lx)",
            static_cast<unsigned long>(descResult));
        return display;
    }

    display.surfaceReady = true;

    bool scrgb = desc.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    ComPtr<IDXGISwapChain3> chain3;
    const HRESULT interfaceResult = native->QueryInterface(IID_PPV_ARGS(chain3.GetAddressOf()));

    if (scrgb) {
        if (!SUCCEEDED(interfaceResult)) {
            qWarning("output: D3D11 scRGB requires IDXGISwapChain3 (HRESULT 0x%08lx), falling back to SDR",
                static_cast<unsigned long>(interfaceResult));
            scrgb = false;
        } else {
            UINT support = 0;
            const HRESULT supportResult
                = chain3->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support);
            if (FAILED(supportResult) || !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
                qWarning("output: D3D11 scRGB color space not supported for present (HRESULT 0x%08lx, support 0x%x), "
                         "falling back to SDR",
                    static_cast<unsigned long>(supportResult), support);
                scrgb = false;
            } else {
                const HRESULT setResult = chain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
                if (FAILED(setResult)) {
                    qWarning("output: D3D11 failed to set scRGB color space (HRESULT 0x%08lx), falling back to SDR",
                        static_cast<unsigned long>(setResult));
                    scrgb = false;
                }
            }
        }
    }

    if (!scrgb && chain3) {
        // Ensure SDR presentation color space on the swapchain.
        chain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    }

    display.hdrAvailable = scrgb;
    display.preferredFormat
        = scrgb ? RenderTargetProfile::Format::ExtendedSrgbLinear : RenderTargetProfile::Format::Sdr;

    HWND hwnd = nullptr;
    if (auto *d3dSwapChain = static_cast<QD3D11SwapChain *>(swapchain)) {
        if (d3dSwapChain->window)
            hwnd = reinterpret_cast<HWND>(d3dSwapChain->window->winId());
    }

    // GetContainingOutput answers only for a swapchain the compositor does not
    // own: it fails with DXGI_ERROR_INVALID_CALL on a composition swapchain,
    // which is what Qt creates when the surface format asks for alpha. The
    // window is opaque for that reason among others -- see
    // platformSurfaceFormat() -- and the display's luminance is readable here
    // because of it.
    ComPtr<IDXGIOutput> output;
    ComPtr<IDXGIOutput6> output6;
    DXGI_OUTPUT_DESC1 outputDesc {};
    const bool outputReported = SUCCEEDED(native->GetContainingOutput(output.GetAddressOf()))
        && SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&outputDesc));

    // This describes Windows' current Advanced Color output, not the app's
    // buffer encoding: Windows composites our scRGB into its HDR/PQ desktop.
    const bool desktopHdr = outputReported && outputDesc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    if (desktopHdr)
        display.supportedFormat = RenderTargetProfile::Format::ExtendedSrgbLinear;
    HMONITOR monitor
        = outputReported ? outputDesc.Monitor : (hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr);

    bool whiteReported = false;
    if (scrgb && monitor) {
        if (const auto white = sdrWhiteNits(monitor)) {
            display.sdrWhiteNits = *white;
            whiteReported = true;
        }
    }
    if (scrgb && outputReported) {
        if (std::isfinite(outputDesc.MaxLuminance) && outputDesc.MaxLuminance > 0.0f) {
            display.maxLuminanceNits = outputDesc.MaxLuminance;
            display.minLuminanceNits = std::isfinite(outputDesc.MinLuminance) && outputDesc.MinLuminance >= 0.0f
                ? outputDesc.MinLuminance
                : 0.0f;
            display.luminanceMeasured = true;
        }
    }
    qInfo("output: D3D11 buffer=%u encoding=%s signaling=%s desktopHdr=%s sdrWhite=%.1f (%s) peak=%.1f (%s)",
        unsigned(desc.BufferDesc.Format), scrgb ? "scRGB BT.709 linear 80-nit unity" : "SDR BT.709 G22",
        chain3 ? "SetColorSpace1" : "DXGI default", outputReported ? (desktopHdr ? "yes" : "no") : "unknown",
        double(display.sdrWhiteNits), whiteReported ? "DisplayConfig" : "fallback", double(display.maxLuminanceNits),
        display.luminanceMeasured ? "DXGI" : "unknown");
    return display;
}

} // namespace JellyfinNative
