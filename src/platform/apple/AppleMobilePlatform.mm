#include "platform/PlatformCapabilities.h"
#include "platform/PlatformSettingsPolicy.h"
#include "platform/PlatformStartup.h"
#include "platform/PlatformSystemProbes.h"
#include "platform/ScreenSaverInhibitor.h"
#include "platform/NativeAppWindow.h"
#include "app/SettingsSchema.h"

#include <QFontDatabase>
#include <mach/mach.h>
#include <sys/sysctl.h>
#import <UIKit/UIKit.h>
#import <TargetConditionals.h>

namespace Spool {
namespace {
    constexpr SettingChoice kAudioChoices[] = { { "auto", "Automatic" } };
    class IdleTimerBackend final : public ScreenSaverBackend {
    public:
        bool acquire() override { set(true); return true; }
        bool release() override { set(false); return true; }
    private:
        static void set(bool inhibited)
        {
            dispatch_async(dispatch_get_main_queue(), ^{ UIApplication.sharedApplication.idleTimerDisabled = inhibited; });
        }
    };
    qint64 physicalMemory()
    {
        uint64_t value = 0;
        size_t size = sizeof(value);
        return sysctlbyname("hw.memsize", &value, &size, nullptr, 0) == 0 ? static_cast<qint64>(value) : 0;
    }
}
const PlatformCapabilities& platformCapabilities()
{
    static const PlatformCapabilities caps {
        .deviceName = QString::fromUtf8(UIDevice.currentDevice.name.UTF8String),
        .rendererName = QStringLiteral("libmpv OpenGL ES"),
#if TARGET_OS_TV
        .isTV = true,
        .isMobile = false,
#else
        .isMobile = true,
#endif
        .hasSystemFonts = true,
        .hasDesktopPointer = false,
#if TARGET_OS_TV
        .hasPointer = false,
#else
        .hasPointer = true,
#endif
        .supportsMpvConfig = false,
    };
    return caps;
}
const PlatformAudioOutputPolicy& platformAudioOutputPolicy()
{
    static const PlatformAudioOutputPolicy policy { kAudioChoices, 1, "auto" };
    return policy;
}
QString normalizedPlatformAudioOutputMode(const QString&) { return QStringLiteral("auto"); }
QStringList platformSystemSubtitleFonts()
{
    auto fonts = QFontDatabase::families();
    fonts.sort(Qt::CaseInsensitive);
    fonts.removeDuplicates();
    return fonts;
}
int platformDefaultUiScalePercent() { return 100; }
const char *platformDefaultArtworkFormat() { return "webp"; }
const char *platformDefaultRenderQuality() { return "balanced"; }
bool platformSupportsDirectVideoOutput() { return false; }
const char *platformDefaultVideoOutput() { return "enhanced"; }
bool platformUsesPerOutputAudioDelay() { return false; }
bool platformDefaultRemoteControlTargetEnabled() { return platformCapabilities().isTV; }
QString normalizedPlatformAudioRoute(const QString& output) { return output; }
QString platformAudioRouteDisplayName(const QString&) { return QStringLiteral("System output"); }
QString platformAudioDelayStorageKey(const QString&) { return QStringLiteral("settings/audioDelayMs"); }
int platformAutomaticAudioDelayMs(const QString&, int, int) { return 0; }
PlatformCpuProbe platformCpuProbe(int logicalCpus)
{
    int physical = 0;
    size_t size = sizeof(physical);
    const bool found = sysctlbyname("hw.physicalcpu", &physical, &size, nullptr, 0) == 0 && physical > 0;
    return { found ? physical : logicalCpus, 0, found ? QStringLiteral("hw.physicalcpu") : QStringLiteral("Qt") };
}
PlatformMemoryPolicy platformMemoryPolicy()
{
    constexpr qint64 mib = 1024LL * 1024LL;
    return { physicalMemory(), 2048 * mib, 96 * mib, 96 * mib, 24, 16 * mib, 64 * mib };
}
QString platformProcessMemoryDiagnostics()
{
    task_vm_info_data_t info {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return {};
    return QStringLiteral("physicalFootprint=%1 resident=%2").arg(info.phys_footprint).arg(info.resident_size);
}
std::unique_ptr<ScreenSaverBackend> createPlatformScreenSaverBackend() { return std::make_unique<IdleTimerBackend>(); }
bool configurePlatformEnvironment(const QString&)
{
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    return true;
}
QSurfaceFormat platformSurfaceFormat()
{
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(3, 0);
    format.setProfile(QSurfaceFormat::NoProfile);
    format.setAlphaBufferSize(8);
    return format;
}
void configurePlatformWindow(NativeAppWindow& window)
{
    window.setFlags(Qt::Window | Qt::FramelessWindowHint);
}
} // namespace Spool
