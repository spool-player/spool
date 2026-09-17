#pragma once

#include <QByteArray>
#include <QString>

#include <vector>

namespace JellyfinNative {

struct MpvOption;

// What the window is presenting into, and what mpv has to be told about it.
//
// The embedded player renders into a target Spool owns, not into a display
// mpv can interrogate. So the transfer function, the primaries and the
// luminance range are decided here and pushed down; reading them back out of
// mpv is how the current OpenGL path ends up reporting SDR for everything.
//
// Three things decide it and they are kept apart on purpose: what the graphics
// backend can present, what the operating system reports about the display,
// and what the user asked for. None of them alone is an answer -- a monitor in
// HDR mode says nothing about whether the backend has an HDR swapchain to
// reach it with.
//
// What the source carries is deliberately not among them. The swapchain is
// chosen when the window is created and Qt gives no way to change it after,
// so a target that followed the file would be a target that cannot exist.
// Source characteristics decide tone mapping and how bright the overlay is
// drawn, which is a different question asked later.
struct RenderTargetProfile {
    enum class Format {
        // 8-bit sRGB. Every backend has this, and it is what the OpenGL
        // framebuffer path can offer.
        Sdr,
        // scRGB: linear, FP16, BT.709 primaries, 1.0 at 80 nits.
        // Values run past 1.0, which is where the highlights live.
        ExtendedSrgbLinear,
        // HDR10: PQ-encoded, BT.2020 primaries, absolute luminance.
        Pq,
    };

    // BT.2408's reference white. The value a diffuse white in SDR content
    // should reach on an HDR display, and what OSD and subtitles are drawn
    // against when the OS reports nothing better.
    static constexpr float kDefaultSdrWhiteNits = 203.0f;

    Format format = Format::Sdr;
    float sdrWhiteNits = kDefaultSdrWhiteNits;
    float minLuminanceNits = 0.0f;
    // Zero means the display did not say. mpv is then left to its own
    // detection rather than told a number nobody measured.
    float maxLuminanceNits = 0.0f;

    bool isHdr() const
    {
        return format != Format::Sdr;
    }
    bool operator==(const RenderTargetProfile& other) const;
    bool operator!=(const RenderTargetProfile& other) const
    {
        return !(*this == other);
    }
};

// What the graphics backend and the operating system between them say the
// window can present. Filled from the live swapchain, never assumed.
struct DisplayOutputCapabilities {
    bool surfaceReady = false;
    // Whether the swapchain in front of us is presenting HDR right now, which
    // is not the same question as whether it could. Qt fixes a window's
    // swapchain format when the window is created, so a backend that supports
    // HDR still presents SDR until it was asked at that moment -- and telling
    // mpv to encode PQ or scRGB into an 8-bit sRGB swapchain is how the
    // picture ends up wrong rather than merely un-improved.
    bool hdrAvailable = false;
    // Qt substitutes Vulkan PASS_THROUGH on Wayland when available. In that
    // case the application, not Mesa's WSI, must retain a surface description.
    bool needsWaylandDescription = false;
    // What the swapchain is actually presenting into. This is what mpv is told
    // about.
    RenderTargetProfile::Format preferredFormat = RenderTargetProfile::Format::Sdr;
    // The best HDR format this backend would accept if the window were created
    // asking for it. Reported so the diagnostics can tell "your display and
    // driver cannot do this" apart from "nothing asked for it", and never used
    // to decide what mpv is told.
    RenderTargetProfile::Format supportedFormat = RenderTargetProfile::Format::Sdr;
    float sdrWhiteNits = RenderTargetProfile::kDefaultSdrWhiteNits;
    float minLuminanceNits = 0.0f;
    float maxLuminanceNits = 0.0f;
    // Whether those numbers came from the display or from Qt's own fixed
    // fallback. Only D3D11, D3D12 and Vulkan-on-Windows ask the system; every
    // other backend returns 1000 nits and 200 nits SDR white regardless of
    // what is plugged in. Handing a guess to mpv as though it were measured is
    // how a display ends up tone mapped to numbers nobody checked.
    bool luminanceMeasured = false;
};

// Viewer overrides take precedence over OS/compositor-reported luminance.
struct RenderTargetOverrides {
    // Nits. Zero leaves whatever the display reported in place.
    float maxLuminanceNits = 0.0f;
    float sdrWhiteNits = 0.0f;
};

// The user's say, which overrides the automatic answer in both directions.
enum class HdrOutputPreference {
    // HDR when the display is in HDR mode and the backend can present it.
    Auto,
    // Never leave SDR, whatever the display says. What someone whose desktop
    // handles HDR badly, or who simply prefers the SDR pipeline, asks for.
    Never,
    // Request HDR even on platforms where automatic selection is not enabled.
    Always,
};

class RenderTargetPolicy final {
public:
    static HdrOutputPreference preferenceFromName(const QString& name);
    static QByteArray preferenceName(HdrOutputPreference preference);

    // Qt reads the swapchain format when the window is first exposed, which is
    // before the application database has answered anything -- and it fixes the
    // format for the life of the window, so a late answer is no answer. So the
    // choice is kept in the platform's own settings store, written whenever the
    // setting changes and read back on the next launch.
    //
    // Returns what to put in the window's "_qt_sg_hdr_format" property, or an
    // empty array to leave the window SDR. Qt ignores a format the display or
    // backend cannot present and falls back to SDR on its own, so asking is
    // safe even where it cannot be granted.
    static QByteArray startupSwapChainRequest(bool automaticHdr = false);
    // Records the choice for the next launch. Takes effect when the window is
    // next created, which is why the setting says as much.
    static void rememberPreference(HdrOutputPreference preference);

    // Which backend the scene graph runs on. It decides what the swapchain can
    // present -- OpenGL has no HDR format to offer at all -- so it is settled
    // in the same place and at the same time as the format above, before
    // QGuiApplication exists, and by the same store.
    enum class GraphicsApiPreference {
        // The platform's own default, which is what every build shipped before
        // this setting existed.
        Automatic,
        OpenGL,
        Direct3D11,
        Vulkan,
    };
    static GraphicsApiPreference graphicsApiFromName(const QString& name);
    static QByteArray graphicsApiName(GraphicsApiPreference preference);
    // What the last launch recorded, or Automatic when nothing has been chosen.
    static GraphicsApiPreference startupGraphicsApi();
    static void rememberGraphicsApi(GraphicsApiPreference preference);

    // The target to present into. Returns an SDR profile whenever HDR was not
    // asked for or is not available.
    static RenderTargetProfile resolve(const DisplayOutputCapabilities& display, HdrOutputPreference preference,
        const RenderTargetOverrides& overrides = {});

    // Complete managed output encoding, including resets for unknown luminance
    // and SDR. These describe our texture, not a user's tone-mapping algorithm.
    static std::vector<MpvOption> targetOptions(const RenderTargetProfile& profile);
};

} // namespace JellyfinNative
