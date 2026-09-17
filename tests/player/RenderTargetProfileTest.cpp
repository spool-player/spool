#include "player/RenderTargetProfile.h"

#include "player/MpvOptionProfile.h"

#include "TestMain.h"

#include <QCoreApplication>
#include <QStandardPaths>

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace JellyfinNative;

namespace {

QByteArray valueFor(const std::vector<MpvOption>& options, const QByteArray& name)
{
    for (const MpvOption& option : options) {
        if (option.name == name)
            return option.value;
    }
    return {};
}

void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(1);
}

DisplayOutputCapabilities hdrDisplay(
    RenderTargetProfile::Format format = RenderTargetProfile::Format::ExtendedSrgbLinear)
{
    DisplayOutputCapabilities display;
    display.hdrAvailable = true;
    display.preferredFormat = format;
    display.sdrWhiteNits = 240.0f;
    display.minLuminanceNits = 0.005f;
    display.maxLuminanceNits = 1000.0f;
    display.luminanceMeasured = true;
    return display;
}

} // namespace

JELLYFIN_TEST_MAIN("render-target-profile")
{
    QCoreApplication app(argc, argv);
    // The startup store is the application's own QSettings, named by these.
    // Test mode puts the file under a throwaway path rather than the
    // developer's own config.
    QStandardPaths::setTestModeEnabled(true);
    app.setOrganizationName(QStringLiteral("spool-jellyfin-test"));
    app.setApplicationName(QStringLiteral("render-target-profile"));

    // Nothing asked, so nothing is requested: a window left alone stays SDR.
    RenderTargetPolicy::rememberPreference(HdrOutputPreference::Auto);
    require(RenderTargetPolicy::startupSwapChainRequest().isEmpty(),
        "automatic should not turn the window over to HDR without automaticHdr enabled");
    require(RenderTargetPolicy::startupSwapChainRequest(true) == QByteArrayLiteral("scrgb"),
        "automatic with automaticHdr enabled should request scrgb");
    RenderTargetPolicy::rememberPreference(HdrOutputPreference::Never);
    require(RenderTargetPolicy::startupSwapChainRequest().isEmpty(), "never should leave the window SDR");
    RenderTargetPolicy::rememberPreference(HdrOutputPreference::Always);
    require(RenderTargetPolicy::startupSwapChainRequest() == QByteArrayLiteral("scrgb"),
        "always should ask Qt for the scRGB swapchain by the name Qt reads");

    const DisplayOutputCapabilities sdrOnly;
    require(!RenderTargetPolicy::resolve(sdrOnly, HdrOutputPreference::Always).isHdr(),
        "a backend with no HDR swapchain stays SDR however loudly it is asked");
    const auto sdrTargetOptions
        = RenderTargetPolicy::targetOptions(RenderTargetPolicy::resolve(sdrOnly, HdrOutputPreference::Auto));
    require(
        valueFor(sdrTargetOptions, "target-trc") == "bt.1886" && valueFor(sdrTargetOptions, "target-prim") == "bt.709",
        "an SDR target should specify bt.709 and bt.1886 target options to reset any prior HDR state");

    require(!RenderTargetPolicy::resolve(hdrDisplay(), HdrOutputPreference::Never).isHdr(),
        "Never should hold an HDR display in SDR");
    require(RenderTargetPolicy::resolve(hdrDisplay(), HdrOutputPreference::Auto).isHdr(),
        "Auto should follow a display that says it is in HDR mode");

    const RenderTargetProfile scrgb = RenderTargetPolicy::resolve(hdrDisplay(), HdrOutputPreference::Auto);
    require(scrgb.format == RenderTargetProfile::Format::ExtendedSrgbLinear && scrgb.isHdr(),
        "an HDR source on an scRGB-capable display should present scRGB");
    const auto scrgbOptions = RenderTargetPolicy::targetOptions(scrgb);
    require(valueFor(scrgbOptions, "target-trc") == "scrgb", "scRGB targets use scRGB transfer");
    require(valueFor(scrgbOptions, "target-prim") == "bt.709", "scRGB encodes extended-range BT.709");
    require(valueFor(scrgbOptions, "target-peak") == "1000", "the display's peak should reach mpv in nits");

    const RenderTargetProfile pq
        = RenderTargetPolicy::resolve(hdrDisplay(RenderTargetProfile::Format::Pq), HdrOutputPreference::Auto);
    const auto pqOptions = RenderTargetPolicy::targetOptions(pq);
    require(valueFor(pqOptions, "target-trc") == "pq" && valueFor(pqOptions, "target-prim") == "bt.2020",
        "an HDR10 target is PQ over BT.2020");

    DisplayOutputCapabilities silent = hdrDisplay();
    silent.maxLuminanceNits = 0.0f;
    silent.sdrWhiteNits = 0.0f;
    const RenderTargetProfile guessed = RenderTargetPolicy::resolve(silent, HdrOutputPreference::Always);
    const auto guessedOptions = RenderTargetPolicy::targetOptions(guessed);
    require(valueFor(guessedOptions, "target-peak") == "auto",
        "a display that reports no peak should set target-peak to auto to reset previous values");
    require(std::fabs(guessed.sdrWhiteNits - RenderTargetProfile::kDefaultSdrWhiteNits) < 0.01f,
        "an unreported SDR white should fall back to the reference value");

    DisplayOutputCapabilities absurd = hdrDisplay();
    absurd.maxLuminanceNits = 250000.0f;
    require(
        valueFor(RenderTargetPolicy::targetOptions(RenderTargetPolicy::resolve(absurd, HdrOutputPreference::Always)),
            "target-peak")
            == "10000",
        "a peak beyond what mpv accepts should be clamped, not passed through and rejected");

    require(RenderTargetPolicy::preferenceFromName(QStringLiteral("Always")) == HdrOutputPreference::Always
            && RenderTargetPolicy::preferenceFromName(QStringLiteral("never")) == HdrOutputPreference::Never
            && RenderTargetPolicy::preferenceFromName(QStringLiteral("nonsense")) == HdrOutputPreference::Auto,
        "the stored preference should round-trip and fall back to Auto");
    require(RenderTargetPolicy::preferenceName(HdrOutputPreference::Always) == "always",
        "the preference should write back the name it reads");

    // Outside Windows nothing measures the panel, so a reported peak is Qt's
    // own fixed guess. Saying nothing leaves mpv to detect, which beats
    // passing 1000 nits off as a description of the display.
    DisplayOutputCapabilities guessing = hdrDisplay();
    guessing.luminanceMeasured = false;
    require(
        valueFor(RenderTargetPolicy::targetOptions(RenderTargetPolicy::resolve(guessing, HdrOutputPreference::Auto)),
            "target-peak")
            == "auto",
        "an unmeasured peak should leave target-peak as auto");

    RenderTargetOverrides manual;
    manual.maxLuminanceNits = 600.0f;
    require(valueFor(RenderTargetPolicy::targetOptions(
                         RenderTargetPolicy::resolve(guessing, HdrOutputPreference::Auto, manual)),
                "target-peak")
            == "600",
        "a peak the viewer set should be used where nothing measured one");
    require(valueFor(RenderTargetPolicy::targetOptions(
                         RenderTargetPolicy::resolve(hdrDisplay(), HdrOutputPreference::Auto, manual)),
                "target-peak")
            == "600",
        "a peak the viewer set should beat even a measured one");

    manual.sdrWhiteNits = 120.0f;
    require(valueFor(RenderTargetPolicy::targetOptions(
                         RenderTargetPolicy::resolve(hdrDisplay(), HdrOutputPreference::Auto, manual)),
                "hdr-reference-white")
            == "120",
        "a reference white the viewer set should win too");
    require(!RenderTargetPolicy::resolve(sdrOnly, HdrOutputPreference::Auto, manual).isHdr(),
        "an override is not a reason to claim an HDR target that does not exist");
    return 0;
}
