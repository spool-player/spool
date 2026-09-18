#include "player/RenderTargetProfile.h"

#include "player/MpvOptionProfile.h"

#include <QSettings>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace JellyfinNative {

namespace {

    // mpv takes both of these in nits and refuses anything outside this range,
    // so a display that reports something absurd is ignored rather than
    // rejected at option-apply time, where the failure is a dead player.
    constexpr float kMinNits = 10.0f;
    constexpr float kMaxNits = 10000.0f;

    bool sameNits(float a, float b)
    {
        return std::fabs(a - b) < 0.01f;
    }

    QByteArray nitsOption(float nits)
    {
        if (!(nits >= kMinNits))
            return {};
        return QByteArray::number(qRound(std::min(nits, kMaxNits)));
    }

} // namespace

bool RenderTargetProfile::operator==(const RenderTargetProfile& other) const
{
    return format == other.format && sameNits(sdrWhiteNits, other.sdrWhiteNits)
        && sameNits(minLuminanceNits, other.minLuminanceNits) && sameNits(maxLuminanceNits, other.maxLuminanceNits);
}

HdrOutputPreference RenderTargetPolicy::preferenceFromName(const QString& name)
{
    const QString normalized = name.trimmed().toLower();
    if (normalized == QStringLiteral("never") || normalized == QStringLiteral("off"))
        return HdrOutputPreference::Never;
    if (normalized == QStringLiteral("always") || normalized == QStringLiteral("on"))
        return HdrOutputPreference::Always;
    return HdrOutputPreference::Auto;
}

namespace {

    // Not the application database: that opens well after the window, and the
    // swapchain format is read once, at the window's first expose. QSettings
    // is synchronous and available before anything else has started.
    constexpr auto kStartupPreferenceKey = "render/hdrOutput";
    constexpr auto kStartupGraphicsApiKey = "render/graphicsApi";

    // The application's own store, by the organisation and application names
    // main() sets before this is ever reached -- not a second file of its own.
    QSettings startupStore()
    {
        return QSettings();
    }

} // namespace

QByteArray RenderTargetPolicy::startupSwapChainRequest(bool automaticHdr)
{
    const auto preference = preferenceFromName(startupStore().value(QLatin1String(kStartupPreferenceKey)).toString());
    if (preference == HdrOutputPreference::Never || (preference == HdrOutputPreference::Auto && !automaticHdr))
        return {};
    // The shell converts SDR UI to linear BT.709 at the reported reference
    // white. Do not request PQ/P3 until their composition paths exist.
    return QByteArrayLiteral("scrgb");
}

void RenderTargetPolicy::rememberPreference(HdrOutputPreference preference)
{
    QSettings store = startupStore();
    store.setValue(QLatin1String(kStartupPreferenceKey), QString::fromLatin1(preferenceName(preference)));
}

RenderTargetPolicy::GraphicsApiPreference RenderTargetPolicy::graphicsApiFromName(const QString& name)
{
    const QString normalized = name.trimmed().toLower();
    if (normalized == QStringLiteral("opengl") || normalized == QStringLiteral("gl"))
        return GraphicsApiPreference::OpenGL;
    if (normalized == QStringLiteral("d3d11") || normalized == QStringLiteral("direct3d11"))
        return GraphicsApiPreference::Direct3D11;
    if (normalized == QStringLiteral("vulkan"))
        return GraphicsApiPreference::Vulkan;
    return GraphicsApiPreference::Automatic;
}

QByteArray RenderTargetPolicy::graphicsApiName(GraphicsApiPreference preference)
{
    switch (preference) {
    case GraphicsApiPreference::OpenGL:
        return QByteArrayLiteral("opengl");
    case GraphicsApiPreference::Direct3D11:
        return QByteArrayLiteral("d3d11");
    case GraphicsApiPreference::Vulkan:
        return QByteArrayLiteral("vulkan");
    case GraphicsApiPreference::Automatic:
        break;
    }
    return QByteArrayLiteral("auto");
}

RenderTargetPolicy::GraphicsApiPreference RenderTargetPolicy::startupGraphicsApi()
{
    return graphicsApiFromName(startupStore().value(QLatin1String(kStartupGraphicsApiKey)).toString());
}

void RenderTargetPolicy::rememberGraphicsApi(GraphicsApiPreference preference)
{
    QSettings store = startupStore();
    store.setValue(QLatin1String(kStartupGraphicsApiKey), QString::fromLatin1(graphicsApiName(preference)));
}

QByteArray RenderTargetPolicy::preferenceName(HdrOutputPreference preference)
{
    switch (preference) {
    case HdrOutputPreference::Never:
        return QByteArrayLiteral("never");
    case HdrOutputPreference::Always:
        return QByteArrayLiteral("always");
    case HdrOutputPreference::Auto:
        break;
    }
    return QByteArrayLiteral("auto");
}

RenderTargetProfile RenderTargetPolicy::resolve(
    const DisplayOutputCapabilities& display, HdrOutputPreference preference, const RenderTargetOverrides& overrides)
{
    const float white = overrides.sdrWhiteNits >= kMinNits ? overrides.sdrWhiteNits : display.sdrWhiteNits;

    RenderTargetProfile profile;
    profile.sdrWhiteNits = white >= kMinNits ? white : RenderTargetProfile::kDefaultSdrWhiteNits;
    if (preference == HdrOutputPreference::Never)
        return profile;
    // No HDR swapchain means no HDR, whatever the display or the user think.
    // Every OpenGL context lands here, which is why this is asked of the
    // backend rather than of the monitor.
    if (!display.hdrAvailable || display.preferredFormat == RenderTargetProfile::Format::Sdr)
        return profile;

    profile.format = display.preferredFormat;
    profile.minLuminanceNits = std::max(0.0f, display.minLuminanceNits);
    // A number the viewer gave wins. Otherwise only a measured one is passed
    // on: where Qt fabricates the luminance, saying nothing leaves mpv to its
    // own detection, which is a better answer than a fixed 1000 nits pretending
    // to describe the panel.
    if (overrides.maxLuminanceNits >= kMinNits)
        profile.maxLuminanceNits = overrides.maxLuminanceNits;
    else if (display.luminanceMeasured)
        profile.maxLuminanceNits = std::max(0.0f, display.maxLuminanceNits);
    return profile;
}

std::vector<MpvOption> RenderTargetPolicy::targetOptions(const RenderTargetProfile& profile)
{
    const bool hdr = profile.isHdr();
    const bool pq = profile.format == RenderTargetProfile::Format::Pq;
    const QByteArray peak = hdr ? nitsOption(profile.maxLuminanceNits) : QByteArray();
    // Describe the embedding target, not a tone-mapping recipe. In particular,
    // reset luminance overrides when returning to SDR or an unknown display.
    return {
        { QByteArrayLiteral("target-prim"), pq ? QByteArrayLiteral("bt.2020") : QByteArrayLiteral("bt.709") },
        { QByteArrayLiteral("target-trc"),
            !hdr     ? QByteArrayLiteral("bt.1886")
                : pq ? QByteArrayLiteral("pq")
                     : QByteArrayLiteral("scrgb") },
        { QByteArrayLiteral("target-peak"), peak.isEmpty() ? QByteArrayLiteral("auto") : peak },
        { QByteArrayLiteral("target-contrast"), hdr ? QByteArrayLiteral("inf") : QByteArrayLiteral("auto") },
        { QByteArrayLiteral("hdr-reference-white"),
            hdr ? nitsOption(profile.sdrWhiteNits) : QByteArrayLiteral("auto") },
    };
}

} // namespace JellyfinNative
