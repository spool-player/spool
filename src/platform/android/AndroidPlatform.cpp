#include "platform/CredentialStore.h"
#include "platform/PlatformCapabilities.h"
#include "platform/PlatformPaths.h"
#include "platform/PlatformSettingsPolicy.h"
#include "platform/PlatformStartup.h"
#include "platform/PlatformSystemProbes.h"
#include "platform/ScreenSaverInhibitor.h"

#include "app/SettingsSchema.h"
#include "platform/NativeAppWindow.h"
#include "platform/common/CredentialStoreFileBackend.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QJniEnvironment>
#include <QJniObject>
#include <QStandardPaths>

#include <algorithm>

namespace JellyfinNative {
namespace {
    constexpr SettingChoice kAndroidAudioChoices[] = { { "auto", "Automatic" } };

    // Which form factor this package is running on. One APK serves handsets
    // and televisions, so this is asked of the system rather than baked in at
    // build time. It is settled once and never changes for the process: the
    // manifest lets the activity survive a uiMode change, but no device turns
    // into a different kind of device while the app is open.
    bool androidIsTelevision()
    {
        const QJniObject context = QNativeInterface::QAndroidApplication::context();
        if (!context.isValid())
            return false;

        // The system's own answer, and the one the platform itself uses to
        // decide which launcher the app belongs in.
        const QJniObject service = QJniObject::fromString(QStringLiteral("uimode"));
        const QJniObject uiMode = context.callObjectMethod(
            "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", service.object<jstring>());
        if (uiMode.isValid()) {
            constexpr jint televisionMode = 4; // Configuration.UI_MODE_TYPE_TELEVISION
            if (uiMode.callMethod<jint>("getCurrentModeType") == televisionMode)
                return true;
        }

        // Some boxes report a normal ui mode and still ship only the leanback
        // launcher, so having the feature at all settles it the other way.
        const QJniObject packages
            = context.callObjectMethod("getPackageManager", "()Landroid/content/pm/PackageManager;");
        if (packages.isValid()) {
            const QJniObject feature = QJniObject::fromString(QStringLiteral("android.software.leanback"));
            if (packages.callMethod<jboolean>("hasSystemFeature", "(Ljava/lang/String;)Z", feature.object<jstring>())) {
                return true;
            }
        }
        return false;
    }

    QString androidDeviceName()
    {
        const QJniObject context = QNativeInterface::QAndroidApplication::context();
        if (context.isValid()) {
            const QJniObject resolver
                = context.callObjectMethod("getContentResolver", "()Landroid/content/ContentResolver;");
            const QJniObject key = QJniObject::fromString(QStringLiteral("device_name"));
            const QJniObject configured = QJniObject::callStaticObjectMethod("android/provider/Settings$Global",
                "getString", "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",
                resolver.object<jobject>(), key.object<jstring>());
            const QString name = configured.toString().trimmed();
            if (!name.isEmpty() && name.compare(QStringLiteral("null"), Qt::CaseInsensitive) != 0)
                return name;
        }

        const QString model
            = QJniObject::getStaticObjectField<jstring>("android/os/Build", "MODEL").toString().trimmed();
        if (!model.isEmpty())
            return model;
        return androidIsTelevision() ? QStringLiteral("Android TV") : QStringLiteral("Android device");
    }

    class AndroidScreenSaverBackend final : public ScreenSaverBackend {
    public:
        bool acquire() override
        {
            return setKeepScreenOn(true);
        }
        bool release() override
        {
            return setKeepScreenOn(false);
        }

    private:
        static bool setKeepScreenOn(bool enabled)
        {
            // Post and return. Waiting on the task deadlocks: this runs on the
            // Qt thread from inside a playback-state signal, and the Android UI
            // thread regularly blocks on the Qt thread while the window changes,
            // so each would be waiting for the other.
            QNativeInterface::QAndroidApplication::runOnAndroidMainThread([enabled] {
                const QJniObject activity = QNativeInterface::QAndroidApplication::context();
                const QJniObject window = activity.callObjectMethod("getWindow", "()Landroid/view/Window;");
                if (!window.isValid())
                    return;
                constexpr jint keepScreenOn = 0x00000080; // WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
                if (enabled)
                    window.callMethod<void>("addFlags", "(I)V", keepScreenOn);
                else
                    window.callMethod<void>("clearFlags", "(I)V", keepScreenOn);
            });
            return true;
        }
    };
}

const PlatformCapabilities& platformCapabilities()
{
    static const PlatformCapabilities capabilities = [] {
        const bool television = androidIsTelevision();
        qInfo() << "android: form factor" << (television ? "television" : "handset");
        PlatformCapabilities probed {
            .deviceName = androidDeviceName(),
            .rendererName = QStringLiteral("libmpv OpenGL ES"),
            .isTV = television,
            .isAndroid = true,
            .isMobile = !television,
            // The Android media stack is built without a system font provider,
            // so libass can only use the fonts the app ships with it.
            .hasSystemFonts = false,
            .hasDesktopPointer = false,
            // A leanback remote is a d-pad; there is nothing there to drag with.
            .hasPointer = !television,
        };
        return probed;
    }();
    return capabilities;
}

QString resolveAppRoot(const char *)
{
    return QStringLiteral(":");
}

QString bundledFontsPath(const QString&)
{
    const QString root
        = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("fonts"));
    QDir().mkpath(root);
    static constexpr const char *fontFiles[] = {
        "AtkinsonHyperlegible-Bold.otf",
        "AtkinsonHyperlegible-Regular.otf",
        "IBMPlexSans-Variable.ttf",
        "PTRootUI-Variable.ttf",
        "MaterialIcons-Regular.ttf",
    };
    for (const char *fontFile : fontFiles) {
        const QString destination = QDir(root).filePath(QString::fromLatin1(fontFile));
        if (QFile::exists(destination))
            continue;
        QFile::copy(QStringLiteral(":/fonts/%1").arg(QString::fromLatin1(fontFile)), destination);
    }
    return root;
}

QString startupCacheRoot(const QString&)
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

QString persistentDataRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QStringList appLogDirectories(const QString&)
{
    return {
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("logs"))
    };
}

QString appLogFileName()
{
    return QStringLiteral("spool.log");
}

const PlatformAudioOutputPolicy& platformAudioOutputPolicy()
{
    static const PlatformAudioOutputPolicy policy { kAndroidAudioChoices, 1, "auto" };
    return policy;
}

QString normalizedPlatformAudioOutputMode(const QString&)
{
    return QStringLiteral("auto");
}

QStringList platformSystemSubtitleFonts()
{
    QStringList families = QFontDatabase::families();
    families.sort(Qt::CaseInsensitive);
    families.removeDuplicates();
    return families;
}

int platformDefaultUiScalePercent()
{
    // Handset dp calibration belongs to Metrics, so the user-facing default
    // is 100%. Keep the existing remote-oriented TV default.
    return platformCapabilities().isTV ? 80 : 100;
}

const char *platformDefaultArtworkFormat()
{
    // A television decodes WebP in software and pays about three times the
    // cost of JPEG for it, which a handset's decoder does not.
    return platformCapabilities().isTV ? "jpeg" : "webp";
}

const char *platformDefaultRenderQuality()
{
    // Television boxes are the weak end of Android by a wide margin: an
    // Amlogic part with a two-core Mali driving a 4K panel. Handsets carry
    // GPUs several classes above that for a screen a fraction of the size.
    return platformCapabilities().isTV ? "fast" : "balanced";
}

bool platformSupportsDirectVideoOutput()
{
    return true;
}

const char *platformDefaultVideoOutput()
{
    // A television box is the case direct output exists for. Handsets have a
    // GPU with room to spare for a screen a fraction of the size, and get the
    // processing that buys.
    return platformCapabilities().isTV ? "direct" : "enhanced";
}

bool platformUsesPerOutputAudioDelay()
{
    return false;
}
bool platformDefaultCastButtonEnabled()
{
    return !platformCapabilities().isTV;
}
bool platformDefaultRemoteControlTargetEnabled()
{
    return platformCapabilities().isTV;
}
QString normalizedPlatformAudioRoute(const QString& output)
{
    return output;
}
QString platformAudioRouteDisplayName(const QString&)
{
    return QStringLiteral("Global");
}
QString platformAudioDelayStorageKey(const QString&)
{
    return QStringLiteral("settings/audioDelayMs");
}
int platformAutomaticAudioDelayMs(const QString&, int, int)
{
    return 0;
}

PlatformCpuProbe platformCpuProbe(int logicalCpus)
{
    return { std::max(1, logicalCpus), 0, QStringLiteral("hardware_concurrency") };
}

PlatformMemoryPolicy platformMemoryPolicy()
{
    constexpr qint64 mib = 1024LL * 1024LL;
    return { 0, 2048LL * mib, 128LL * mib, 128LL * mib, 32, 24LL * mib, 96LL * mib };
}

qint64 effectiveLinuxMemoryBytes(const QByteArray&, const QByteArray&, const QByteArray&)
{
    return 0;
}

QString platformProcessMemoryDiagnostics()
{
    return {};
}

std::unique_ptr<ScreenSaverBackend> createPlatformScreenSaverBackend()
{
    return std::make_unique<AndroidScreenSaverBackend>();
}

// libmpv statically links FFmpeg and re-exports this. FFmpeg's MediaCodec
// decoders and mpv's AudioTrack output both reach Android through JNI and
// refuse to start until a virtual machine has been registered, and nothing in
// libmpv registers one — that is the embedding application's job.
extern "C" int av_jni_set_java_vm(void *vm, void *log_ctx);

bool configurePlatformEnvironment(const QString&)
{
    qputenv("QT_QUICK_CONTROLS_STYLE", QByteArrayLiteral("Basic"));
    if (JavaVM *vm = QJniEnvironment::javaVM())
        av_jni_set_java_vm(vm, nullptr);
    else
        qWarning() << "android: no Java VM to register; hardware decoding is unavailable";
    return true;
}

QSurfaceFormat platformSurfaceFormat()
{
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(3, 0);
    format.setProfile(QSurfaceFormat::NoProfile);
    return format;
}

void configurePlatformWindow(NativeAppWindow& window)
{
    // Qt's Android surface asks for setZOrderMediaOverlay when the window
    // stays on top, which is what puts the interface above the video plane
    // direct playback adds beneath it. Set before the window is shown,
    // because Qt reads the flags when it creates the surface.
    window.setFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
}

namespace CredentialStore {
    namespace {
        void ensureCredentialRoot()
        {
            if (qEnvironmentVariableIsEmpty("JELLYFIN_CREDENTIAL_STORE_DIR")) {
                const QString root = QDir(persistentDataRoot()).filePath(QStringLiteral("credentials"));
                qputenv("JELLYFIN_CREDENTIAL_STORE_DIR", root.toUtf8());
            }
        }
    }

    QString load(const QString& profileId)
    {
        ensureCredentialRoot();
        return FileBackend::load(profileId);
    }
    bool save(const QString& profileId, const QString& accessToken)
    {
        ensureCredentialRoot();
        return FileBackend::save(profileId, accessToken);
    }
    void remove(const QString& profileId)
    {
        ensureCredentialRoot();
        FileBackend::remove(profileId);
    }
    void clear()
    {
        ensureCredentialRoot();
        FileBackend::clear();
    }
} // namespace CredentialStore

} // namespace JellyfinNative
