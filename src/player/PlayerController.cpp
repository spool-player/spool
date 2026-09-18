#include "PlayerController.h"

#include "../common/LogRotation.h"
#include "../common/TlsTrust.h"
#include "../diagnostics/Diagnostics.h"
#include "../media/MediaTypes.h"
#include "../platform/MpvConfigPolicy.h"
#include "../platform/NativeAppWindow.h"
#include "../platform/PlatformPaths.h"
#include "../platform/PlatformPlaybackSurface.h"
#include "../platform/PlatformSystemProbes.h"
#include "../provider/PlaybackSource.h"
#include "MpvOptionProfile.h"
#include "PlaybackFailurePolicy.h"
#include "PlaybackTrackParser.h"

extern "C" {
#include <mpv/client.h>
}

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMetaObject>
#include <QPointer>
#include <QUrl>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

namespace JellyfinNative {

namespace {

    // Warnings and errors are what a player problem report needs;
    // SPOOL_MPV_LOG_LEVEL accepts any level mpv understands when more detail is
    // wanted.
    const char *mpvLogLevel()
    {
        static const QByteArray level = qEnvironmentVariableIsSet("SPOOL_MPV_LOG_LEVEL")
            ? qgetenv("SPOOL_MPV_LOG_LEVEL")
            : QByteArrayLiteral("warn");
        return level.constData();
    }

    constexpr auto kMpvLogFileName = "spool-jellyfin-mpv.log";

    constexpr uint64_t kTimePosRefreshReply = 0x6a666e7074730001ULL;
    constexpr auto kNightModeFilter
        = "lavfi=[pan=stereo|FL<0.5*FL+1.0*FC+0.25*BL|FR<0.5*FR+1.0*FC+0.25*BR,"
          "dialoguenhance=original=0.25:enhance=2.0,"
          "pan=stereo|FL=FL+0.6*FC|FR=FR+0.6*FC,"
          "compand=attacks=0.02:decays=0.5:points=-80/-80|-45/-35|-30/-25|-20/-18|0/-10:gain=2,"
          "highpass=f=50:p=2:t=q:w=0.7071,"
          "equalizer=f=60:t=q:w=1.4:g=-4,"
          "equalizer=f=98:t=q:w=6.0:g=-9,"
          "equalizer=f=131:t=q:w=2.5:g=-11,"
          "equalizer=f=850:t=q:w=3.0:g=-1,"
          "equalizer=f=2000:t=q:w=2.0:g=5,"
          "equalizer=f=3200:t=q:w=2.5:g=4.5,"
          "equalizer=f=4200:t=q:w=2.0:g=3.5,"
          "treble=f=7500:t=q:w=0.6667:g=3,"
          "speechnorm=e=12.5:r=0.0001:l=1,"
          "alimiter=limit=0.95:attack=3:release=50]";

    const char *endFileReasonName(int reason)
    {
        switch (reason) {
        case MPV_END_FILE_REASON_EOF:
            return "eof";
        case MPV_END_FILE_REASON_STOP:
            return "stop";
        case MPV_END_FILE_REASON_QUIT:
            return "quit";
        case MPV_END_FILE_REASON_ERROR:
            return "error";
        case MPV_END_FILE_REASON_REDIRECT:
            return "redirect";
        default:
            return "unknown";
        }
    }

    QByteArray mpvLogPath()
    {
        const QByteArray logDir = qgetenv("JELLYFIN_NATIVE_LOG_DIR");
        if (logDir.isEmpty()) {
            const QString fallback = startupCacheRoot({});
            return QFile::encodeName(QDir(fallback).filePath(QString::fromLatin1(kMpvLogFileName)));
        }

        QByteArray path = logDir;
        if (!path.endsWith('/'))
            path += '/';
        path += QByteArray(kMpvLogFileName);
        return path;
    }

    QByteArray mpvShaderCachePath()
    {
        const QString cacheDirectory = QDir(startupCacheRoot({})).filePath(QStringLiteral("mpv-shaders"));
        return QFile::encodeName(cacheDirectory);
    }

    bool setOption(mpv_handle *handle, const char *name, const char *value)
    {
        const int error = mpv_set_option_string(handle, name, value);
        if (error >= 0 || error == MPV_ERROR_OPTION_NOT_FOUND)
            return true;
        qWarning() << "player: failed to set mpv option" << name << "=" << value << mpv_error_string(error);
        return false;
    }

    bool applyOptions(mpv_handle *handle, const std::vector<MpvOption>& options)
    {
        bool ok = true;
        for (const MpvOption& option : options)
            ok &= setOption(handle, option.name.constData(), option.value.constData());
        return ok;
    }
    bool setMpvProperty(mpv_handle *handle, const char *name, const char *value)
    {
        const int error = mpv_set_property_string(handle, name, value);
        return error >= 0 || error == MPV_ERROR_OPTION_NOT_FOUND;
    }

    bool setRequiredMpvProperty(mpv_handle *handle, const char *name, const char *value)
    {
        return mpv_set_property_string(handle, name, value) >= 0;
    }

    bool setMpvDoubleProperty(mpv_handle *handle, const char *name, double value, double *appliedValue = nullptr)
    {
        const int error = mpv_set_property(handle, name, MPV_FORMAT_DOUBLE, &value);
        if (error < 0 && error != MPV_ERROR_OPTION_NOT_FOUND)
            return false;

        if (appliedValue)
            *appliedValue = value;

        double readback = 0.0;
        const int readError = mpv_get_property(handle, name, MPV_FORMAT_DOUBLE, &readback);
        if (readError >= 0 && appliedValue)
            *appliedValue = readback;

        return true;
    }

    QByteArray mpvBool(bool value)
    {
        return value ? QByteArrayLiteral("yes") : QByteArrayLiteral("no");
    }

    qint64 secondsToTicks(double seconds)
    {
        return static_cast<qint64>(seconds * 10000000.0);
    }

    void logMemoryStats()
    {
        const QString diagnostics = platformProcessMemoryDiagnostics();
        if (!diagnostics.isEmpty())
            qInfo().noquote() << "player: memstats" << diagnostics;
    }

} // namespace

PlayerController::PlayerController(NativeAppWindow *window, PlaybackSource *api, TlsTrustController *tlsTrust,
    const QString& subtitleFontsPath, QObject *parent)
    : QObject(parent)
    , m_window(window)
    , m_api(api)
    , m_reporter(api, this)
    , m_tlsTrust(tlsTrust)
    , m_subtitleFontsPath(QFile::encodeName(subtitleFontsPath))
{
    if (m_window) {
        connect(m_window, &QWindow::activeChanged, this, [this]() {
            if (!m_window->isActive())
                releaseMpvKeys();
        });
        connect(m_window, &NativeAppWindow::fullScreenChanged, this, [this]() {
            if (platformMpvOptionProfile() == MpvOptionProfile::Platform::Desktop) {
                if (auto *handle = m_mpvLifecycle.handle())
                    setMpvProperty(handle, "fullscreen", m_window->fullScreen() ? "yes" : "no");
            }
        });
    }
    if (m_api) {
        connect(m_api, &PlaybackSource::credentialsChanged, this, [this]() {
            if (auto *handle = m_mpvLifecycle.handle())
                setMpvProperty(handle, "http-header-fields", "");
        });
    }
    m_idleMpvPreparationEnabled = platformIdleMpvPreparationEnabled();
    m_progressTimer.setInterval(5000);
    m_uiPositionTimer.setInterval(250);
    m_backGuardTimer.setSingleShot(true);
    m_backGuardTimer.setInterval(1500);
    m_seekWatchdogTimer.setSingleShot(true);
    m_seekWatchdogTimer.setInterval(2500);
    m_backgroundTeardownTimer.setSingleShot(true);
    m_backgroundTeardownTimer.setInterval(750);
    // Five seconds of real playback is enough to tell a device that cannot
    // keep up from one that merely stuttered while the cache filled, and it
    // is short enough that the viewer is still watching the opening titles
    // when the answer arrives.
    m_renderStrainTimer.setSingleShot(true);
    m_renderStrainTimer.setInterval(5000);
    connect(&m_renderStrainTimer, &QTimer::timeout, this, [this]() {
        if (!m_sessionActive || m_renderStrainReported || !m_embeddedVideoOutput)
            return;
        if (!m_fileLoaded || m_paused || m_buffering || m_seeking) {
            m_renderStrainTimer.start();
            return;
        }
        // Counting dropped frames was not enough, and the Chromecast proved
        // it: a 4K HDR file played visibly slow, the picture dragging behind
        // the sound, while both drop counters sat at zero. With video synced
        // to audio, a renderer that cannot keep up does not drop frames -- it
        // presents them late and simply runs slow. So the question asked here
        // is whether the picture is actually arriving at the rate the file
        // says it should.
        constexpr qint64 tolerableDrops = 10;
        constexpr qint64 tolerableDelays = 10;
        constexpr double sustainedFraction = 0.85;
        const bool tooSlow
            = m_containerFps > 0.0 && m_outputFps > 0.0 && m_outputFps < m_containerFps * sustainedFraction;
        // Decoder drops are a different illness -- a stream the hardware
        // cannot decode -- and no amount of cheaper rendering helps them, so
        // they are reported but never acted on.
        const bool strained = tooSlow || m_outputDroppedFrames > tolerableDrops || m_delayedFrames > tolerableDelays;
        qInfo() << "player: opening-seconds render check output-drops=" << m_outputDroppedFrames
                << "decoder-drops=" << m_decoderDroppedFrames << "late=" << m_delayedFrames << "fps=" << m_outputFps
                << "of" << m_containerFps
                << "quality=" << MpvOptionProfile::renderQualityName(m_renderQuality).constData()
                << "strained=" << strained;
        if (!strained)
            return;
        m_renderStrainReported = true;
        emit renderQualityStrained(m_outputDroppedFrames + m_delayedFrames);
    });
    connect(&m_backGuardTimer, &QTimer::timeout, this, [this]() {
        if (!m_sessionActive || m_backAllowed)
            return;
        m_backAllowed = true;
        qInfo() << "player: startup back guard released";
        emit playbackStateChanged();
    });
    connect(&m_uiPositionTimer, &QTimer::timeout, this, [this]() {
        if (m_sessionActive)
            emit positionChanged();
    });
    connect(&m_seekWatchdogTimer, &QTimer::timeout, this, [this]() {
        if (!m_sessionActive || !(m_seeking || m_positionTracker.seekInFlight()))
            return;

        // mpv coalesces seeks queued in the same iteration into one restart, so
        // a seek can be waiting for a confirmation that will never arrive. Give
        // its position back to mpv rather than leaving the seek bar pinned to a
        // target nothing is going to confirm.
        qInfo() << "player: clearing stale seek state";
        m_seeking = false;
        m_positionTracker.abandonSeeks();
        requestMpvPositionRefresh("seek watchdog");
        notifyPlaybackStateChanged();
    });
    connect(&m_progressTimer, &QTimer::timeout, this, [this]() {
        if (!m_sessionActive)
            return;

        logMemoryStats();

        m_reporter.reportProgress(secondsToTicks(m_positionTracker.position()), m_paused, effectivePlaybackSpeed(),
            m_volume.load(), m_muted.load());
    });
    connect(&m_reporter, &PlaybackReporter::reportFailed, this, [](const QString& operation, const QString& message) {
        Diagnostics::logEvent(QStringLiteral("player"), QStringLiteral("report_failed"),
            { { QStringLiteral("operation"), operation }, { QStringLiteral("message"), message } });
    });
    if (m_api) {
        connect(m_api, &PlaybackSource::playbackNetworkProfileChanged, this,
            [this]() { discardPreparedMpvForOptionChange("network profile change"); });
    }
    scheduleIdleMpvPreparation();
}

PlayerController::~PlayerController()
{
    teardownMpv();
}

void PlayerController::prepareForShutdown()
{
    m_idleMpvPreparationEnabled = false;
    m_idleMpvPreparationScheduled = false;
    if (!m_mpvLifecycle.handle())
        return;

    // Silence first: reporting shutdown may cancel network work, but it must
    // never keep audible playback alive while the application is closing.
    mpvCommand(
        { QByteArrayLiteral("no-osd"), QByteArrayLiteral("set"), QByteArrayLiteral("mute"), QByteArrayLiteral("yes") });
    if (m_sessionActive)
        stopWithReason(QStringLiteral("app-shutdown"));
    else
        mpvCommand({ QByteArrayLiteral("stop") });
}

void PlayerController::teardownMpv(bool async)
{
    releaseMpvKeys();
    ++m_mpvTeardownGeneration;
    Diagnostics::Phase phase(QStringLiteral("shutdown"), QStringLiteral("player_teardown_mpv"));
    m_idleMpvPreparationEnabled = false;
    m_idleMpvPreparationScheduled = false;
    destroyIdleMpv("teardown");
    // Platform render resources must detach before the mpv core is destroyed.
    if (!releasePlatformMpvSurface(m_embeddedVideoOutput)) {
        qCritical() << "player: timed out releasing the mpv render context; preserving the mpv core";
        return;
    }
    // The deferred post-stop teardown must not stall the GUI thread; shutdown
    // and the stale-core path before a new play() stay synchronous so the new
    // pipeline never races the old core for media resources.
    if (async)
        m_mpvLifecycle.destroyAsync();
    else
        m_mpvLifecycle.destroy();
    m_embeddedVideoOutput = false;
    m_videoWidth = 0;
    m_videoHeight = 0;
}

void PlayerController::scheduleIdleMpvPreparation()
{
    if (!m_idleMpvPreparationEnabled || m_idleMpvPreparationScheduled || m_idleMpvHandle || m_mpvLifecycle.handle())
        return;

    m_idleMpvPreparationScheduled = true;
    QPointer<PlayerController> controller(this);
    runAfterPlatformMpvLoaded([controller]() {
        if (auto *app = QCoreApplication::instance()) {
            // Delay past the launch window: idle preparation saves little at
            // play-start and must not compete with first-page construction.
            QTimer::singleShot(3000, app, [controller]() {
                if (controller)
                    controller->prepareIdleMpv();
            });
        }
    });
}

void PlayerController::prepareIdleMpv()
{
    m_idleMpvPreparationScheduled = false;
    if (!m_idleMpvPreparationEnabled || m_idleMpvHandle || m_mpvLifecycle.handle() || m_sessionActive)
        return;

    QElapsedTimer startupTimer;
    startupTimer.start();
    const QByteArray logPath = mpvLogPath();
    rotateLogFile(logPath.constData());
    mpv_handle *handle = mpv_create();
    if (!handle) {
        qWarning() << "player: idle mpv_create failed";
        return;
    }

    if (!configureAndInitializeMpv(handle, false)) {
        mpv_terminate_destroy(handle);
        qWarning() << "player: idle mpv initialization failed";
        return;
    }

    m_idleMpvHandle = handle;
    qInfo() << "player: idle-prepared mpv initialized in" << startupTimer.elapsed() << "ms";
}

void PlayerController::destroyIdleMpv(const char *reason)
{
    if (!m_idleMpvHandle)
        return;

    mpv_handle *handle = m_idleMpvHandle;
    m_idleMpvHandle = nullptr;
    qInfo() << "player: destroying idle-prepared mpv" << (reason ? reason : "unspecified");
    mpv_terminate_destroy(handle);
}

mpv_handle *PlayerController::takeIdleMpvHandle()
{
    mpv_handle *handle = m_idleMpvHandle;
    m_idleMpvHandle = nullptr;
    if (handle)
        qInfo() << "player: adopting idle-prepared mpv handle";
    return handle;
}

bool PlayerController::configureAndInitializeMpv(mpv_handle *handle, bool embeddedVideo)
{
    if (!handle)
        return false;

    const auto platform = platformMpvOptionProfile();
    const int parallelRequests = m_api ? m_api->playbackParallelRequests() : 1;
    const MpvOptionProfile::NetworkProfile network = MpvOptionProfile::networkProfile(platform, parallelRequests);
    qInfo().nospace() << "player: curl profile source=MpvOptionProfile platform="
                      << platformPlaybackBackendName(embeddedVideo) << " requestsPerStream=" << network.parallelRequests
                      << " rangeBytes=" << network.rangeBytes << " ringBytes=" << network.ringBytes;
    // Qt's trust first: it resolves the platform store through the OS rather
    // than a path chosen when libcurl was compiled, and it is the only place a
    // certificate the viewer remembered can reach playback. The path probe
    // stays behind it for the case where the bundle cannot be written.
    QByteArray certificateBundle = QFile::encodeName(writeTrustBundle());
    if (certificateBundle.isEmpty())
        certificateBundle = MpvOptionProfile::systemCertificateBundle();
    if (certificateBundle.isEmpty())
        qWarning() << "player: no certificate bundle; playback TLS will use libcurl's built-in trust";
    auto applicationOptions = MpvOptionProfile::applicationOptions(platform, m_audioOutputMode, mpvLogPath(),
        m_demuxerMaxBytes, m_demuxerMaxBackBytes, parallelRequests, embeddedVideo, mpvShaderCachePath(),
        certificateBundle, m_renderQuality);
    applicationOptions.push_back({ "sub-fonts-dir", m_subtitleFontsPath });
    // mpv's OSD — the performance stats overlay among it — is drawn by libass
    // too, and it looks for fonts under its own options rather than the
    // subtitle ones. Builds without a system font provider found nothing at
    // all and drew an empty overlay, so name the bundled family the app
    // already ships for subtitles.
    applicationOptions.push_back({ "osd-fonts-dir", m_subtitleFontsPath });
    applicationOptions.push_back({ "osd-font", QByteArrayLiteral("IBM Plex Sans Var") });

    mpv_request_log_messages(handle, mpvLogLevel());
    if (!applyOptions(handle, MpvOptionProfile::preInitializeOptions(m_mpvConfigPolicy))
        || !applyOptions(handle, applicationOptions))
        return false;
    if (usesUserMpvConfig() && !applyMpvRuntimeOptions(MpvOptionApplyMode::Initial, handle))
        return false;
    int initializeResult;
#if !defined(JELLYFIN_NATIVE_WEBOS) && !defined(Q_OS_ANDROID)
    // Command-line precedence is applied by mpv after its own config parser,
    // before scripts or force-window can create a native player window.
    char embeddingOptions[][40] = {
        "--vo=libmpv",
        "--gpu-api=opengl",
        "--gpu-context=auto",
        "--wid=-1",
        "--force-window=no",
        "--idle=yes",
        "--keep-open=no",
        "--input-vo-keyboard=no",
        "--input-cursor=no",
        "--terminal=no",
        "--osc=no",
    };
    char *embeddingArguments[std::size(embeddingOptions) + 1] {};
    for (size_t i = 0; i < std::size(embeddingOptions); ++i)
        embeddingArguments[i] = embeddingOptions[i];
    if (usesUserMpvConfig())
        qInfo() << "player: user mpv config enabled; embedding overrides vo, gpu-api, gpu-context,"
                   " wid, force-window, idle, keep-open, input-vo-keyboard, input-cursor, terminal and osc";
    initializeResult = mpv_initialize_opts(handle, embeddingArguments);
#else
    initializeResult = mpv_initialize(handle);
#endif
    if (initializeResult < 0)
        return false;
    // mpv's own log file lives in application-private storage, which is
    // unreadable on Android. Mirror its messages into the app log so player
    // problems are diagnosable wherever the app runs.
    mpv_request_log_messages(handle, mpvLogLevel());

    if (platform == MpvOptionProfile::Platform::Desktop) {
        for (const char *name : { "mpv-version", "ffmpeg-version", "hwdec", "gpu-api", "gpu-context" }) {
            char *value = mpv_get_property_string(handle, name);
            qInfo() << "player: initialized mpv" << name << (value ? value : "unavailable");
            mpv_free(value);
        }
    }
    return usesUserMpvConfig() || applyMpvRuntimeOptions(MpvOptionApplyMode::Runtime, handle);
}

bool PlayerController::usesUserMpvConfig() const
{
    if (m_mpvLifecycle.handle())
        return m_activeUserMpvConfig;
    return platformMpvOptionProfile() == MpvOptionProfile::Platform::Desktop
        && m_mpvConfigPolicy.mode != MpvConfigPolicy::Mode::Disabled;
}

bool PlayerController::forwardMpvKey(int key, int modifiers, const QString& text, bool pressed, bool repeat)
{
    auto *handle = m_mpvLifecycle.handle();
    if (platformMpvOptionProfile() != MpvOptionProfile::Platform::Desktop || !handle || !m_sessionActive)
        return false;
    if (repeat)
        return m_mpvKeys.contains(key); // mpv owns repeat timing; Qt repeat releases are synthetic.
    if (pressed && m_mpvKeys.contains(key))
        return true;
    QByteArray name = pressed ? MpvOptionProfile::inputKey(key, modifiers, text) : m_mpvKeys.value(key);
    if (name.isEmpty())
        return false;
    const char *command[] = { pressed ? "keydown" : "keyup", name.constData(), nullptr };
    if (mpv_command_async(handle, 0, command) < 0)
        return false;
    if (pressed)
        m_mpvKeys.insert(key, name);
    else
        m_mpvKeys.remove(key);
    return true;
}

void PlayerController::releaseMpvKeys()
{
    if (m_mpvKeys.isEmpty())
        return;
    if (auto *handle = m_mpvLifecycle.handle()) {
        const char *command[] = { "keyup", nullptr };
        mpv_command_async(handle, 0, command);
    }
    m_mpvKeys.clear();
}

void PlayerController::observeMpvProperties(mpv_handle *handle)
{
    mpv_observe_property(handle, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(handle, 0, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(handle, 0, "cache-buffering-state", MPV_FORMAT_INT64);
    mpv_observe_property(handle, 0, "seeking", MPV_FORMAT_FLAG);
    mpv_observe_property(handle, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(handle, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(handle, 0, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(handle, 0, "track-list", MPV_FORMAT_NODE);
    mpv_observe_property(handle, 0, "chapter-list", MPV_FORMAT_NODE);
    mpv_observe_property(handle, 0, "chapter", MPV_FORMAT_INT64);
    mpv_observe_property(handle, 0, "video-params/transfer", MPV_FORMAT_STRING);
    mpv_observe_property(handle, 0, "video-target-params/transfer", MPV_FORMAT_STRING);
    mpv_observe_property(handle, 0, "hwdec-current", MPV_FORMAT_STRING);
    if (platformMpvOptionProfile() == MpvOptionProfile::Platform::Desktop) {
        for (const char *name : { "current-vo", "current-gpu-context", "video-codec", "video-dec-params/pixelformat" })
            mpv_observe_property(handle, 0, name, MPV_FORMAT_STRING);
        if (m_window)
            setMpvProperty(handle, "fullscreen", m_window->fullScreen() ? "yes" : "no");
        mpv_observe_property(handle, 0, "fullscreen", MPV_FORMAT_FLAG);
        mpv_observe_property(handle, 0, "speed", MPV_FORMAT_DOUBLE);
        mpv_observe_property(handle, 0, "mute", MPV_FORMAT_FLAG);
    }
    mpv_observe_property(handle, 0, "decoder-frame-drop-count", MPV_FORMAT_INT64);
    mpv_observe_property(handle, 0, "frame-drop-count", MPV_FORMAT_INT64);
    mpv_observe_property(handle, 0, "vo-delayed-frame-count", MPV_FORMAT_INT64);
    mpv_observe_property(handle, 0, "estimated-vf-fps", MPV_FORMAT_DOUBLE);
    mpv_observe_property(handle, 0, "container-fps", MPV_FORMAT_DOUBLE);
    // Display size rather than storage size: it is already through the
    // aspect-ratio correction, which is what a video plane has to match.
    mpv_observe_property(handle, 0, "dwidth", MPV_FORMAT_INT64);
    mpv_observe_property(handle, 0, "dheight", MPV_FORMAT_INT64);
}

void PlayerController::scheduleMpvTeardown()
{
    auto *scheduledHandle = m_mpvLifecycle.handle();
    if (!scheduledHandle)
        return;

    const quint64 scheduledGeneration = ++m_mpvTeardownGeneration;
    QTimer::singleShot(1000, this, [this, scheduledHandle, scheduledGeneration]() {
        if (m_mpvTeardownGeneration != scheduledGeneration || m_mpvLifecycle.handle() != scheduledHandle)
            return;
        qInfo() << "player: deferred mpv teardown";
        teardownMpv(true);
    });
}

bool PlayerController::visible() const
{
    return m_visible;
}

bool PlayerController::sessionActive() const
{
    return m_sessionActive;
}

bool PlayerController::fileLoaded() const
{
    return m_fileLoaded;
}
bool PlayerController::hdrPlayback() const
{
    return m_hdrPlayback;
}
void PlayerController::updateHdrOutput(bool applySubtitleOptions)
{
    const bool hdrOutput = MpvOptionProfile::isHdrOutput(m_starfishVideoOutput, m_hdrInput, m_targetTransfer);
    if (m_hdrPlayback == hdrOutput)
        return;
    m_hdrPlayback = hdrOutput;
    emit hdrPlaybackChanged();
    if (applySubtitleOptions && !usesUserMpvConfig()) {
        if (auto *handle = m_mpvLifecycle.handle())
            applyMpvSubtitleOptions(MpvOptionApplyMode::Runtime, handle, true);
    }
    qInfo() << "player: HDR paperwhite" << (m_hdrPlayback ? "enabled" : "disabled") << "inputHdr=" << m_hdrInput
            << "starfish=" << m_starfishVideoOutput << "targetTransfer=" << m_targetTransfer;
}

QString PlayerController::mediaKind() const
{
    return m_mediaKind;
}

QString PlayerController::mediaKindForSession(const PlaybackSession& session)
{
    bool hasAudio = false;
    for (const MediaStreamInfo& stream : session.mediaStreams) {
        if (stream.type.compare(QStringLiteral("Video"), Qt::CaseInsensitive) == 0)
            return QStringLiteral("video");
        if (stream.type.compare(QStringLiteral("Audio"), Qt::CaseInsensitive) == 0)
            hasAudio = true;
    }
    if (hasAudio || session.itemType.compare(QStringLiteral("Audio"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("audio");
    return QStringLiteral("video");
}

bool PlayerController::paused() const
{
    return m_paused;
}

QString PlayerController::title() const
{
    return m_title;
}

QString PlayerController::statusText() const
{
    return m_statusText;
}

QString PlayerController::errorText() const
{
    return m_errorText;
}

bool PlayerController::buffering() const
{
    return m_buffering;
}

int PlayerController::bufferingPercent() const
{
    return m_bufferingPercent;
}

bool PlayerController::seeking() const
{
    return m_seeking;
}

bool PlayerController::debugOsdVisible() const
{
    return m_debugOsdVisible;
}

bool PlayerController::embeddedVideoOutput() const
{
    return m_embeddedVideoOutput;
}

qint64 PlayerController::decoderDroppedFrames() const
{
    return m_decoderDroppedFrames;
}

qint64 PlayerController::outputDroppedFrames() const
{
    return m_outputDroppedFrames;
}

bool PlayerController::subtitlesEnabled() const
{
    return m_tracks.subtitlesEnabled();
}

QStringList PlayerController::subtitleTracks() const
{
    return m_tracks.subtitleTracks();
}

int PlayerController::selectedSubtitleIndex() const
{
    return m_tracks.selectedSubtitleIndex();
}

QStringList PlayerController::audioTracks() const
{
    return m_tracks.audioTracks();
}

int PlayerController::selectedAudioIndex() const
{
    return m_tracks.selectedAudioIndex();
}

bool PlayerController::backAllowed() const
{
    return m_backAllowed;
}

double PlayerController::positionSeconds() const
{
    return estimatedPositionSeconds();
}

double PlayerController::estimatedPositionSeconds() const
{
    const bool advancing = m_sessionActive && !m_paused && !m_buffering && !m_seeking;
    return m_positionTracker.estimatedPosition(effectivePlaybackSpeed(), advancing);
}

double PlayerController::durationSeconds() const
{
    return m_positionTracker.duration();
}

QVariantList PlayerController::chapters() const
{
    return m_tracks.chapters();
}

bool PlayerController::hasChapters() const
{
    return m_tracks.hasChapters();
}

int PlayerController::currentChapter() const
{
    return m_tracks.currentChapter();
}

bool PlayerController::nightModeEnabled() const
{
    return m_nightModeEnabled.load();
}

bool PlayerController::toneMappingVisualizationEnabled() const
{
    return m_toneMappingVisualizationEnabled.load();
}

int PlayerController::audioDelayMs() const
{
    return m_audioDelayMs.load();
}

int PlayerController::fileAudioDelayMs() const
{
    return m_fileAudioDelayMs.load();
}

int PlayerController::effectiveAudioDelayMs() const
{
    return qBound(-4000, m_audioDelayMs.load() + m_fileAudioDelayMs.load(), 4000);
}

int PlayerController::subtitleDelayMs() const
{
    return m_subtitleDelayMs.load();
}

QString PlayerController::audioOutputMode() const
{
    return m_audioOutputMode;
}

int PlayerController::volume() const
{
    return m_volume.load();
}
bool PlayerController::muted() const
{
    return m_muted.load();
}

double PlayerController::playbackSpeed() const
{
    return m_playbackSpeed;
}

double PlayerController::effectivePlaybackSpeed() const
{
    return m_syncPlaybackSpeedActive ? m_syncPlaybackSpeed : m_playbackSpeed;
}

bool PlayerController::applyMpvRuntimeOption(MpvRuntimeOption option, MpvOptionApplyMode mode, mpv_handle *handle)
{
    if (!handle)
        return false;

    const char *name = nullptr;
    QByteArray value;
    double doubleValue = 0.0;
    switch (option) {
    case MpvRuntimeOption::NightMode:
        name = "af";
        value = m_nightModeEnabled.load() ? QByteArray(kNightModeFilter) : QByteArray();
        break;
    case MpvRuntimeOption::ToneMappingVisualization:
        name = "tone-mapping-visualize";
        value = mpvBool(m_toneMappingVisualizationEnabled.load());
        break;
    case MpvRuntimeOption::AudioDelay:
        name = "audio-delay";
        doubleValue = static_cast<double>(effectiveAudioDelayMs()) / 1000.0;
        value = QByteArray::number(doubleValue, 'f', 3);
        break;
    case MpvRuntimeOption::SubtitleDelay:
        name = "sub-delay";
        doubleValue = static_cast<double>(m_subtitleDelayMs.load()) / 1000.0;
        value = QByteArray::number(doubleValue, 'f', 3);
        break;
    case MpvRuntimeOption::PlaybackSpeed:
        name = "speed";
        doubleValue = effectivePlaybackSpeed();
        value = QByteArray::number(doubleValue, 'f', 3);
        break;
    }

    double appliedDoubleValue = doubleValue;
    const bool numericOption = option == MpvRuntimeOption::AudioDelay || option == MpvRuntimeOption::SubtitleDelay
        || option == MpvRuntimeOption::PlaybackSpeed;
    const bool ok = mode == MpvOptionApplyMode::Initial ? setOption(handle, name, value.constData())
        : numericOption ? setMpvDoubleProperty(handle, name, doubleValue, &appliedDoubleValue)
                        : setMpvProperty(handle, name, value.constData());
    if (!ok) {
        qWarning() << "player: failed to apply mpv runtime option" << name
                   << "mode=" << (mode == MpvOptionApplyMode::Initial ? "initial" : "runtime");
    } else if (numericOption) {
        qInfo() << "player: applied numeric playback option" << name
                << "mode=" << (mode == MpvOptionApplyMode::Initial ? "initial" : "runtime")
                << "requested=" << doubleValue << "applied=" << appliedDoubleValue;
    }
    return ok;
}

bool PlayerController::applyMpvRuntimeOptions(MpvOptionApplyMode mode, mpv_handle *handle)
{
    return applyMpvRuntimeOption(MpvRuntimeOption::NightMode, mode, handle)
        && applyMpvRuntimeOption(MpvRuntimeOption::ToneMappingVisualization, mode, handle)
        && applyMpvRuntimeOption(MpvRuntimeOption::AudioDelay, mode, handle)
        && applyMpvRuntimeOption(MpvRuntimeOption::SubtitleDelay, mode, handle)
        && applyMpvRuntimeOption(MpvRuntimeOption::PlaybackSpeed, mode, handle)
        && applyMpvSubtitleOptions(mode, handle);
}

void PlayerController::discardPreparedMpvForOptionChange(const char *reason)
{
    if (m_mpvLifecycle.handle())
        return;

    destroyIdleMpv(reason);
    scheduleIdleMpvPreparation();
}

bool PlayerController::applyMpvSubtitleOptions(MpvOptionApplyMode mode, mpv_handle *handle, bool preserveTrackSelection,
    const SubtitlePreferences *previousPreferences)
{
    if (!handle)
        return false;

    auto applyString = [mode, handle, preserveTrackSelection](const char *name, const QByteArray& value) {
        if (mode == MpvOptionApplyMode::Initial)
            return setOption(handle, name, value.constData());
        if (!preserveTrackSelection)
            return setMpvProperty(handle, name, value.constData());

        const char *raw = value.constData();
        return mpv_set_property_async(handle, 0, name, MPV_FORMAT_STRING, &raw) >= 0;
    };

    bool ok = true;
    const auto options
        = MpvOptionProfile::subtitleOptions(m_subtitlePreferences, m_tracks.subtitlesEnabled(), m_hdrPlayback);
    const auto previousOptions = previousPreferences
        ? MpvOptionProfile::subtitleOptions(*previousPreferences, m_tracks.subtitlesEnabled(), m_hdrPlayback)
        : std::vector<MpvOption> {};
    for (const MpvOption& option : options) {
        const auto previous = std::find_if(previousOptions.begin(), previousOptions.end(),
            [&option](const MpvOption& candidate) { return candidate.name == option.name; });
        if (previous != previousOptions.end() && previous->value == option.value)
            continue;
        const bool selectsTrack = option.name == QByteArrayLiteral("sid") || option.name == QByteArrayLiteral("slang")
            || option.name == QByteArrayLiteral("alang") || option.name == QByteArrayLiteral("sub-auto")
            || option.name == QByteArrayLiteral("sub-visibility")
            || option.name == QByteArrayLiteral("sub-forced-events-only")
            || option.name == QByteArrayLiteral("subs-with-matching-audio")
            || option.name == QByteArrayLiteral("subs-fallback")
            || option.name == QByteArrayLiteral("subs-fallback-forced");
        if (!preserveTrackSelection || !selectsTrack)
            ok &= applyString(option.name.constData(), option.value);
    }

    if (!ok) {
        qWarning() << "player: failed to apply subtitle preferences"
                   << "mode=" << (mode == MpvOptionApplyMode::Initial ? "initial" : "runtime");
    } else {
        qInfo() << (mode == MpvOptionApplyMode::Runtime && preserveTrackSelection
                ? "player: subtitle appearance queued"
                : "player: subtitle appearance applied")
                << "mode=" << (mode == MpvOptionApplyMode::Initial ? "initial" : "runtime")
                << "preserveTrack=" << preserveTrackSelection << "hdr=" << m_hdrPlayback
                << "brightnessPercent=" << m_subtitlePreferences.hdrBrightnessPercent;
    }
    return ok;
}

bool PlayerController::ensureMpv(bool needsVideoSurface, bool embeddedVideo)
{
    if (m_mpvLifecycle.handle())
        return true;

    QElapsedTimer startupTimer;
    startupTimer.start();

    if (embeddedVideo)
        destroyIdleMpv("embedded software video output");
    mpv_handle *handle = takeIdleMpvHandle();
    const bool idlePrepared = handle != nullptr;
    if (!handle) {
        const QByteArray logPath = mpvLogPath();
        rotateLogFile(logPath.constData());
        handle = mpv_create();
        if (!handle) {
            m_errorText = QStringLiteral("mpv_create failed.");
            emit playbackStateChanged();
            return false;
        }

        if (!configureAndInitializeMpv(handle, embeddedVideo)) {
            mpv_terminate_destroy(handle);
            m_errorText = QStringLiteral("Failed to initialize libmpv.");
            emit playbackStateChanged();
            return false;
        }
    }

    QString surfaceError;
    if (!configurePlatformMpvSurface(handle, *m_window, needsVideoSurface, embeddedVideo, surfaceError)) {
        mpv_terminate_destroy(handle);
        m_errorText = surfaceError;
        emit playbackStateChanged();
        return false;
    }

    observeMpvProperties(handle);

    qInfo() << "player: mpv initialized in" << startupTimer.elapsed() << "ms"
            << "idlePrepared=" << idlePrepared;

    QString attachmentError;
    if (!attachPlatformMpvSurface(
            handle, needsVideoSurface, embeddedVideo, *this,
            [this](const QString& message) { handleVideoRenderError(message); }, attachmentError)) {
        mpv_terminate_destroy(handle);
        m_errorText = attachmentError;
        m_statusText = QStringLiteral("Playback unavailable");
        emit playbackStateChanged();
        return false;
    }

    m_embeddedVideoOutput = needsVideoSurface && embeddedVideo;
    m_activeUserMpvConfig = usesUserMpvConfig();
    if (!m_mpvLifecycle.adopt(handle, [this](mpv_event *event) { handleMpvEvent(event); })) {
        if (needsVideoSurface)
            releasePlatformMpvSurface(m_embeddedVideoOutput);
        m_embeddedVideoOutput = false;
        mpv_terminate_destroy(handle);
        m_errorText = QStringLiteral("Failed to start the libmpv event loop.");
        emit playbackStateChanged();
        return false;
    }
    return true;
}

void PlayerController::handleVideoRenderError(const QString& message)
{
    m_errorText = message;
    m_statusText = QStringLiteral("Playback unavailable");
    emit playbackStateChanged();
}

void PlayerController::play(const PlaybackSession& session, bool startPaused)
{
    const QString nextMediaKind = mediaKindForSession(session);
    const bool needsVideoSurface = nextMediaKind == QStringLiteral("video");
    const bool embeddedVideo = needsVideoSurface && platformUsesEmbeddedVideo(session, m_directVideoOutput);
    Diagnostics::Task task(QStringLiteral("player_play"),
        { { QStringLiteral("itemId"), session.itemId }, { QStringLiteral("title"), session.title },
            { QStringLiteral("mediaKind"), nextMediaKind } });
    qInfo() << "player: play requested" << session.title << "method=" << session.playMethod
            << "mediaKind=" << nextMediaKind << "startTimeTicks=" << session.startTimeTicks
            << "startPaused=" << startPaused;

    if (m_mpvLifecycle.handle()) {
        qInfo() << "player: tearing down stale mpv before play";
        teardownMpv();
    }
    resetRenderStrain();

    const bool hadFileAudioDelay = m_fileAudioDelayMs.exchange(0) != 0;
    const bool hadSubtitleDelay = m_subtitleDelayMs.exchange(0) != 0;
    if (hadFileAudioDelay) {
        emit fileAudioDelayMsChanged();
        emit effectiveAudioDelayMsChanged();
    }
    if (hadSubtitleDelay)
        emit subtitleDelayMsChanged();

    m_hdrInput = MpvOptionProfile::isHdrPlayback(session.mediaStreams);
    m_starfishVideoOutput
        = needsVideoSurface && !embeddedVideo && platformMpvOptionProfile() == MpvOptionProfile::Platform::WebOS;
    m_targetTransfer.clear();
    updateHdrOutput(false);
    qInfo() << "player: HDR input metadata" << m_hdrInput
            << "outputPolicy=" << (m_starfishVideoOutput ? "starfish-assumed" : "detected");
    m_window->clearOverlay();
    if (needsVideoSurface && !embeddedVideo) {
        QElapsedTimer playbackSurfaceTimer;
        playbackSurfaceTimer.start();
        if (!m_window->prepareForPlaybackSurface()) {
            m_errorText = QStringLiteral("Failed to prepare the native playback surface.");
            qWarning() << "player: prepareForPlaybackSurface failed after" << playbackSurfaceTimer.elapsed() << "ms";
            emit playbackStateChanged();
            return;
        }
        qInfo() << "player: prepareForPlaybackSurface completed in" << playbackSurfaceTimer.elapsed() << "ms";
    } else if (embeddedVideo) {
        qInfo() << "player: using embedded OpenGL software video surface";
    } else {
        qInfo() << "player: audio-only playback does not request a video surface";
    }

    if (!ensureMpv(needsVideoSurface, embeddedVideo))
        return;

    m_session = session;
    m_timeline.setSession(session);
    rebuildTrickplaySheetUrls();
    m_title = session.title;
    m_mediaKind = nextMediaKind;
    m_statusText = platformPreparingStatus(needsVideoSurface, embeddedVideo);
    m_errorText.clear();
    const double startSeconds
        = session.startTimeTicks > 0 ? static_cast<double>(session.startTimeTicks) / 10000000.0 : 0.0;
    // Seed the position and runtime from the session so a restart, such as a
    // quality change, never shows a seek bar snapped to zero on its way back
    // to where the viewer was.
    m_positionTracker.reset(startSeconds);
    if (session.runtimeTicks > 0)
        m_positionTracker.setDuration(static_cast<double>(session.runtimeTicks) / 10000000.0);
    m_paused = startPaused;
    m_fileLoaded = false;
    m_seekDispatchReady = false;
    m_buffering = false;
    m_bufferingPercent = 0;
    m_seeking = false;
    m_pendingSeek = false;
    m_pendingSeekFlags.clear();
    m_tracks.resetForPlayback();
    m_restoreStreamSelection = session.restoreStreamSelection;
    m_backAllowed = false;
    m_backGuardTimer.start();
    m_uiPositionTimer.start();
    const bool wasVisible = m_visible;
    const bool wasSessionActive = m_sessionActive;
    // Audio has no video surface but still owns the screen: it gets the same
    // transport chrome over a now playing stage instead of playing unseen.
    m_visible = true;
    m_sessionActive = true;
    if (wasVisible != m_visible)
        emit visibleChanged();
    if (wasSessionActive != m_sessionActive)
        emit sessionActiveChanged();
    emit positionChanged();
    emit playbackStateChanged();
    emit tracksChanged();
    emit segmentsChanged();
    emit trickplayChanged();

    QString surfaceReadyError;
    if (!waitForPlatformMpvSurfaceReady(needsVideoSurface, embeddedVideo, surfaceReadyError)) {
        m_errorText = surfaceReadyError;
        m_statusText = QStringLiteral("Playback unavailable");
        teardownMpv();
        emit playbackStateChanged();
        return;
    }
    auto *handle = m_mpvLifecycle.handle();
    // An idle-prepared mpv was configured before this session's HDR policy
    // was known. Reapply subtitle options now so HDR paperwhite is correct
    // from the first rendered subtitle, not only after a settings change.
    if (!usesUserMpvConfig() && !applyMpvSubtitleOptions(MpvOptionApplyMode::Runtime, handle)) {
        m_mpvLifecycle.cancelFileLoad();
        m_errorText = QStringLiteral("libmpv rejected the subtitle appearance.");
        stopProgressReporting(true);
        return;
    }

    m_mpvLifecycle.beginFileLoad();

    // SyncPlay queue preparation must never emit audio or advance the
    // timeline before the server's scheduled Unpause command. Set pause on
    // the idle mpv core before loadfile so even the first decoded frame is
    // held. Ordinary playback explicitly clears any inherited pause state.
    if (!setRequiredMpvProperty(handle, "pause", startPaused ? "yes" : "no")) {
        m_mpvLifecycle.cancelFileLoad();
        m_errorText = QStringLiteral("libmpv rejected the initial playback state.");
        stopProgressReporting(true);
        return;
    }

    QString subtitlePreloadError;
    if (!applyPlatformSubtitlePreload(handle, session, m_subtitlePreferences.language, subtitlePreloadError)) {
        m_mpvLifecycle.cancelFileLoad();
        m_errorText = subtitlePreloadError;
        stopProgressReporting(true);
        return;
    }

    const QByteArray urlBytes = session.url.toUtf8();
    const QByteArray header = m_api ? m_api->mediaRequestHeaders() : QByteArray {};
    if (!setRequiredMpvProperty(handle, "http-header-fields", header.constData())) {
        m_mpvLifecycle.cancelFileLoad();
        m_errorText = QStringLiteral("libmpv rejected the authenticated media request.");
        stopProgressReporting(true);
        return;
    }
    if (!setRequiredMpvProperty(handle, "tls-verify", "yes")) {
        m_mpvLifecycle.cancelFileLoad();
        m_errorText = QStringLiteral("libmpv could not enable secure certificate verification.");
        stopProgressReporting(true);
        return;
    }
    if (m_api && m_tlsTrust) {
        const QSslCertificate certificate = m_tlsTrust->trustedCertificate(m_api->mediaOrigin());
        if (!certificate.isNull()) {
            const QString trustDirectory = QDir(startupCacheRoot({})).filePath(QStringLiteral("tls"));
            const QString trustPath = trustDirectory + QLatin1Char('/')
                + QString::fromLatin1(TlsTrustController::fingerprint(certificate)) + QStringLiteral(".pem");
            const QByteArray certificatePem = certificate.toPem();
            QDir().mkpath(trustDirectory);
            QFile trustFile(trustPath);
            if ((!trustFile.exists() || trustFile.size() == 0)
                && (!trustFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
                    || trustFile.write(certificatePem) != certificatePem.size())) {
                m_mpvLifecycle.cancelFileLoad();
                m_errorText = QStringLiteral("The trusted server certificate could not be prepared for playback.");
                stopProgressReporting(true);
                return;
            }
            trustFile.close();
            const QByteArray encodedTrustPath = QFile::encodeName(trustPath);
            if (!setRequiredMpvProperty(handle, "tls-ca-file", encodedTrustPath.constData())) {
                m_mpvLifecycle.cancelFileLoad();
                m_errorText = QStringLiteral("libmpv rejected the trusted server certificate.");
                stopProgressReporting(true);
                return;
            }
        }
    }
    if (startSeconds > 0.0) {
        const QByteArray startValue = QByteArray::number(startSeconds, 'f', 3);
        if (!setOption(handle, "start", startValue.constData())) {
            m_mpvLifecycle.cancelFileLoad();
            m_errorText = QStringLiteral("libmpv rejected the resume position.");
            stopProgressReporting(true);
            return;
        }
        qInfo() << "player: instructing mpv to start at resume position seconds=" << startSeconds;
    }
    const QByteArray loadFileOptions = MpvOptionProfile::loadFileOptions(session);
    const char *loadCommand[] = { "loadfile", urlBytes.constData(), "replace", "-1",
        loadFileOptions.isEmpty() ? nullptr : loadFileOptions.constData(), nullptr };
    if (mpv_command(handle, loadCommand) < 0) {
        m_mpvLifecycle.cancelFileLoad();
        m_errorText = QStringLiteral("libmpv rejected the playback URL.");
        stopProgressReporting(true);
        return;
    }
}

void PlayerController::setMediaSegments(const QString& itemId, const std::vector<MediaSegment>& segments)
{
    if (!m_sessionActive || itemId != m_session.itemId)
        return;

    m_session.segments = segments;
    m_timeline.setSession(m_session);
    m_timeline.updatePosition(m_positionTracker.position());
    qInfo() << "player: media segments updated" << itemId << "count=" << segments.size();
    emit segmentsChanged();
}

void PlayerController::togglePause()
{
    qInfo() << "player: toggle pause requested";
    mpvCommand({ QByteArrayLiteral("no-osd"), QByteArrayLiteral("cycle"), QByteArrayLiteral("pause") });
}

void PlayerController::setPaused(bool paused)
{
    qInfo() << "player: pause requested" << paused;
    mpvCommand({ QByteArrayLiteral("no-osd"), QByteArrayLiteral("set"), QByteArrayLiteral("pause"),
        paused ? QByteArrayLiteral("yes") : QByteArrayLiteral("no") });
}

void PlayerController::prepareForBackground()
{
    if (!platformUsesBackgroundPlaybackPolicy())
        return;
    if (!m_sessionActive) {
        m_idleMpvPreparationEnabled = false;
        m_idleMpvPreparationScheduled = false;
        destroyIdleMpv("background");
        return;
    }
    qInfo() << "player: playback position snapshot background"
            << "position=" << m_positionTracker.position();
}

void PlayerController::teardownForBackground()
{
    if (!platformUsesBackgroundPlaybackPolicy())
        return;
    prepareForBackground();
    if (!m_sessionActive)
        return;
    // A transient hidden state during surface handoff must not tear down
    // ordinary playback. A real background transition outlasts this timer,
    // at which point Starfish must release the system media pipeline.
    qInfo() << "player: scheduling teardown for background/hidden app state";
    m_backgroundTeardownTimer.start();
}

void PlayerController::resyncForForeground()
{
    if (!platformUsesBackgroundPlaybackPolicy())
        return;
    if (m_backgroundTeardownTimer.isActive()) {
        m_backgroundTeardownTimer.stop();
        qInfo() << "player: cancelled transient background teardown";
    }
    if (!m_visible)
        return;

    qInfo() << "player: foreground position resync requested";
    requestMpvPositionRefresh("foreground");

    for (int delayMs : { 250, 1000, 2500 }) {
        QTimer::singleShot(delayMs, this, [this]() {
            if (!m_visible)
                return;
            requestMpvPositionRefresh("foreground-delayed");
        });
    }
}

void PlayerController::seekBack()
{
    beginRelativeSeekCommand(-10.0);
}

void PlayerController::seekForward()
{
    beginRelativeSeekCommand(10.0);
}

void PlayerController::seek(double seconds)
{
    if (!std::isfinite(seconds))
        return;

    const double clampedSeconds = m_positionTracker.clamp(seconds);
    // Use absolute+exact so a committed click lands on the requested frame.
    qInfo() << "player: absolute exact seek" << clampedSeconds;
    beginSeekCommand(clampedSeconds, QByteArray("absolute+exact"));
}

void PlayerController::previewSeekBy(double deltaSeconds)
{
    beginRelativeSeekCommand(deltaSeconds);
}

namespace {

    // mpv's own stats page, laid out the way its stats script lays it out and
    // formatted by mpv's property expansion, so the readings are mpv's rather
    // than a re-derivation of them. Audio playback runs without a video output
    // and so without an OSD to draw the real page on; this is how it reaches a
    // window that has to paint the page itself.
    constexpr const char *kStatsTemplate = "${?filename:File: ${filename}\n}"
                                           "${?file-format:Format: ${file-format}}"
                                           "${?file-size:   Size: ${file-size}}"
                                           "${?duration:   Length: ${duration}}\n"
                                           "${?time-pos:Position: ${time-pos}"
                                           "${?percent-pos:   ${percent-pos}%}\n}"
                                           "${?video-codec:Video: ${video-codec}"
                                           "${?video-params/w:   ${video-params/w}x${video-params/h}}"
                                           "${?container-fps:   ${container-fps} fps}"
                                           "${?hwdec-current:   hwdec ${hwdec-current}}\n}"
                                           "${?audio-codec:Audio: ${audio-codec}\n}"
                                           "${?audio-params/format:   Decoded: ${audio-params/format}"
                                           "   ${audio-params/samplerate} Hz   ${audio-params/hr-channels}\n}"
                                           "${?current-ao:   Output: ${current-ao}}"
                                           "${?audio-out-params/format:   ${audio-out-params/format}"
                                           "   ${audio-out-params/samplerate} Hz   ${audio-out-params/hr-channels}}\n"
                                           "${?audio-bitrate:   Bitrate: ${audio-bitrate}}"
                                           "${?avsync:   A-V: ${avsync}}"
                                           "${?speed:   Speed: ${speed}x}\n"
                                           "${?demuxer-cache-duration:Cache: ${demuxer-cache-duration} s}"
                                           "${?demuxer-cache-state/fw-bytes:   ${demuxer-cache-state/fw-bytes} ahead}"
                                           "${?cache-speed:   ${cache-speed}}\n"
                                           "${?decoder-frame-drop-count:Dropped: ${decoder-frame-drop-count} decoder}"
                                           "${?frame-drop-count:   ${frame-drop-count} output}";

    QString expandMpvStatsPage(mpv_handle *handle)
    {
        const char *args[] = { "expand-text", kStatsTemplate, nullptr };
        mpv_node result {};
        if (mpv_command_ret(handle, args, &result) < 0)
            return QString();
        const QString text
            = result.format == MPV_FORMAT_STRING && result.u.string ? QString::fromUtf8(result.u.string) : QString();
        mpv_free_node_contents(&result);
        return text.trimmed();
    }

} // namespace

double PlayerController::seekAnchorSeconds()
{
    return seekAnchorPosition();
}

QString PlayerController::mpvStatsPage()
{
    auto *handle = m_mpvLifecycle.handle();
    if (!handle || !m_sessionActive)
        return QString();
    return expandMpvStatsPage(handle);
}

void PlayerController::toggleDebugOsd()
{
    // mpv's stats page is drawn by the video output. Audio playback has none,
    // so the flag alone is what matters there: the UI draws the page instead.
    const bool mpvDrawsStats = m_mediaKind == QStringLiteral("video");
    if (mpvDrawsStats) {
        if (!mpvCommand({ QByteArrayLiteral("script-binding"), QByteArrayLiteral("stats/display-stats-toggle") }))
            return;
    } else if (!m_sessionActive) {
        return;
    }
    m_debugOsdVisible = !m_debugOsdVisible;
    emit playbackStateChanged();
}

void PlayerController::toggleSubtitles()
{
    if (const auto target = m_tracks.toggleSubtitleTarget())
        selectSubtitle(*target);
}

void PlayerController::cycleSubtitles()
{
    // Off (index 0) -> first track -> second -> ... -> last -> Off.
    if (const auto target = m_tracks.cycleSubtitleTarget())
        selectSubtitle(*target);
}

void PlayerController::enableSubtitles()
{
    if (const auto target = m_tracks.enableSubtitleTarget())
        selectSubtitle(*target);
}

void PlayerController::cycleAudio()
{
    if (const auto target = m_tracks.cycleAudioTarget())
        selectAudio(*target);
}

void PlayerController::selectSubtitle(int index)
{
    const std::optional<QByteArrayList> command = m_tracks.subtitleCommand(index);
    if (!command)
        return;

    mpvCommand(*command);
    m_tracks.applySubtitleSelection(index);
    if (!m_tracks.subtitlesEnabled()) {
        m_window->clearOverlay();
    }
    updateReportedStreamSelection(true);
    emit streamSelectionChanged(m_session.audioStreamIndex, m_session.subtitleStreamIndex);
    emit tracksChanged();
}

void PlayerController::selectSubtitleStreamIndex(int streamIndex)
{
    const int uiIndex = streamIndex < 0 ? 0 : uiTrackIndexForStream(QStringLiteral("Subtitle"), streamIndex, 1);
    if (uiIndex < 0) {
        qWarning() << "player: Jellyfin subtitle stream index not found" << streamIndex;
        return;
    }
    qInfo() << "player: selecting Jellyfin subtitle stream" << streamIndex << "uiIndex" << uiIndex;
    selectSubtitle(uiIndex);
}

void PlayerController::selectAudio(int index)
{
    const std::optional<QByteArrayList> command = m_tracks.audioCommand(index);
    if (!command)
        return;

    if (!mpvCommand(*command))
        return;
    m_tracks.applyAudioSelection(index);
    updateReportedStreamSelection(true);
    emit streamSelectionChanged(m_session.audioStreamIndex, m_session.subtitleStreamIndex);
    platformAudioTrackChanged(index);
    emit tracksChanged();
}

void PlayerController::selectAudioStreamIndex(int streamIndex)
{
    const int uiIndex = uiTrackIndexForStream(QStringLiteral("Audio"), streamIndex, 0);
    if (uiIndex < 0) {
        qWarning() << "player: Jellyfin audio stream index not found" << streamIndex;
        return;
    }
    qInfo() << "player: selecting Jellyfin audio stream" << streamIndex << "uiIndex" << uiIndex;
    selectAudio(uiIndex);
}

int PlayerController::uiTrackIndexForStream(const QString& type, int streamIndex, int firstUiIndex) const
{
    int uiIndex = firstUiIndex;
    for (const MediaStreamInfo& stream : m_session.mediaStreams) {
        if (stream.type.compare(type, Qt::CaseInsensitive) != 0)
            continue;
        if (stream.index == streamIndex)
            return uiIndex;
        ++uiIndex;
    }
    return -1;
}

int PlayerController::streamIndexForUiTrack(const QString& type, int uiIndex, int firstUiIndex) const
{
    if (uiIndex < firstUiIndex)
        return -1;
    int candidateUiIndex = firstUiIndex;
    for (const MediaStreamInfo& stream : m_session.mediaStreams) {
        if (stream.type.compare(type, Qt::CaseInsensitive) != 0)
            continue;
        if (candidateUiIndex == uiIndex)
            return stream.index;
        ++candidateUiIndex;
    }
    return -1;
}

void PlayerController::updateReportedStreamSelection(bool sendProgress)
{
    m_session.audioStreamIndex = streamIndexForUiTrack(QStringLiteral("Audio"), m_tracks.selectedAudioIndex(), 0);
    m_session.subtitleStreamIndex = m_tracks.subtitlesEnabled()
        ? streamIndexForUiTrack(QStringLiteral("Subtitle"), m_tracks.selectedSubtitleIndex(), 1)
        : -1;
    const bool reportChanged = m_reporter.setStreamIndexes(m_session.audioStreamIndex, m_session.subtitleStreamIndex);
    if (reportChanged && sendProgress && m_sessionActive)
        m_reporter.reportProgress(secondsToTicks(m_positionTracker.position()), m_paused, effectivePlaybackSpeed(),
            m_volume.load(), m_muted.load());
}

void PlayerController::stepChapter(int delta)
{
    if (!m_tracks.hasChapters())
        return;
    // Where a chapter step lands is mpv's to say, so the position stands until
    // it says so -- but it is still a seek, and the samples on the way to it
    // belong to where playback was, not to where it is going.
    m_positionTracker.beginBlindSeek();
    m_seekWatchdogTimer.start();
    if (mpvCommand({ QByteArrayLiteral("add"), QByteArrayLiteral("chapter"), QByteArray::number(delta) }))
        return;
    m_seekWatchdogTimer.stop();
    m_positionTracker.abandonSeeks();
}

void PlayerController::nextChapter()
{
    stepChapter(1);
}

void PlayerController::previousChapter()
{
    stepChapter(-1);
}

void PlayerController::stop()
{
    stopWithReason(QStringLiteral("unspecified"));
}

void PlayerController::stopWithReason(const QString& reason)
{
    Diagnostics::Task task(QStringLiteral("player_stop"),
        { { QStringLiteral("reason"), reason }, { QStringLiteral("sessionActive"), m_sessionActive } });
    qInfo() << "player: stop requested" << reason << "sessionActive" << m_sessionActive;
    if (!m_sessionActive)
        return;

    // Drop the UI synchronously so navigation never waits for backend unload.
    stopProgressReporting(false);

    if (auto *handle = m_mpvLifecycle.handle())
        setMpvProperty(handle, "http-header-fields", "");
    mpvCommand({ QByteArrayLiteral("stop") });
    scheduleMpvTeardown();
}

void PlayerController::setNightModeEnabled(bool enabled)
{
    if (m_nightModeEnabled.load() == enabled)
        return;

    m_nightModeEnabled = enabled;
    if (auto *handle = m_mpvLifecycle.handle()) {
        applyMpvRuntimeOption(MpvRuntimeOption::NightMode, MpvOptionApplyMode::Runtime, handle);
    } else {
        discardPreparedMpvForOptionChange("night mode change");
    }

    emit nightModeEnabledChanged();
}

void PlayerController::setToneMappingVisualizationEnabled(bool enabled)
{
    if (m_toneMappingVisualizationEnabled.load() == enabled)
        return;

    m_toneMappingVisualizationEnabled = enabled;
    if (auto *handle = m_mpvLifecycle.handle()) {
        applyMpvRuntimeOption(MpvRuntimeOption::ToneMappingVisualization, MpvOptionApplyMode::Runtime, handle);
    } else {
        discardPreparedMpvForOptionChange("tone mapping visualization change");
    }

    emit toneMappingVisualizationEnabledChanged();
}

void PlayerController::setAudioDelayMs(int delayMs)
{
    const int clampedDelayMs = qBound(-2000, delayMs, 2000);
    if (m_audioDelayMs.load() == clampedDelayMs) {
        qInfo() << "player: audio delay unchanged" << clampedDelayMs << "ms";
        return;
    }

    m_audioDelayMs = clampedDelayMs;
    qInfo() << "player: audio delay requested" << clampedDelayMs << "ms"
            << "visible=" << m_visible;
    if (auto *handle = m_mpvLifecycle.handle()) {
        applyMpvRuntimeOption(MpvRuntimeOption::AudioDelay, MpvOptionApplyMode::Runtime, handle);
    } else {
        qInfo() << "player: audio delay stored without active mpv";
        discardPreparedMpvForOptionChange("audio delay change");
    }

    emit audioDelayMsChanged();
    emit effectiveAudioDelayMsChanged();
}

void PlayerController::setFileAudioDelayMs(int delayMs)
{
    const int clampedDelayMs = qBound(-2000, delayMs, 2000);
    if (m_fileAudioDelayMs.exchange(clampedDelayMs) == clampedDelayMs)
        return;
    if (auto *handle = m_mpvLifecycle.handle())
        applyMpvRuntimeOption(MpvRuntimeOption::AudioDelay, MpvOptionApplyMode::Runtime, handle);
    emit fileAudioDelayMsChanged();
    emit effectiveAudioDelayMsChanged();
}

void PlayerController::setSubtitleDelayMs(int delayMs)
{
    const int clampedDelayMs = qBound(-2000, delayMs, 2000);
    if (m_subtitleDelayMs.exchange(clampedDelayMs) == clampedDelayMs)
        return;
    if (auto *handle = m_mpvLifecycle.handle())
        applyMpvRuntimeOption(MpvRuntimeOption::SubtitleDelay, MpvOptionApplyMode::Runtime, handle);
    emit subtitleDelayMsChanged();
}

void PlayerController::setAudioOutputMode(const QString& mode)
{
    const QString normalized = normalizedAudioOutputMode(mode);
    if (m_audioOutputMode == normalized)
        return;

    m_audioOutputMode = normalized;
    qInfo() << "player: audio output mode changed" << normalized << "visible=" << m_visible;
    discardPreparedMpvForOptionChange("audio output mode change");
    emit audioOutputModeChanged();
}

void PlayerController::setVolume(int volume)
{
    const int clampedVolume = qBound(0, volume, 100);
    if (m_volume.load() == clampedVolume)
        return;

    m_volume = clampedVolume;
    mpvCommand({ QByteArrayLiteral("no-osd"), QByteArrayLiteral("set"), QByteArrayLiteral("volume"),
        QByteArray::number(clampedVolume) });
    emit volumeChanged();
    if (m_sessionActive)
        m_reporter.reportProgress(secondsToTicks(m_positionTracker.position()), m_paused, effectivePlaybackSpeed(),
            m_volume.load(), m_muted.load());
}

void PlayerController::adjustVolume(int delta)
{
    if (delta == 0)
        return;
    setVolume(m_volume.load() + delta);
}
void PlayerController::setMuted(bool muted)
{
    if (m_muted.load() == muted)
        return;
    m_muted = muted;
    mpvCommand({ QByteArrayLiteral("no-osd"), QByteArrayLiteral("set"), QByteArrayLiteral("mute"),
        muted ? QByteArrayLiteral("yes") : QByteArrayLiteral("no") });
    emit volumeChanged();
    if (m_sessionActive)
        m_reporter.reportProgress(secondsToTicks(m_positionTracker.position()), m_paused, effectivePlaybackSpeed(),
            m_volume.load(), m_muted.load());
}

void PlayerController::toggleMuted()
{
    setMuted(!m_muted.load());
}

void PlayerController::setPlaybackSpeed(double speed)
{
    changePlaybackSpeed(speed, false);
}

void PlayerController::setSyncPlaybackSpeed(double speed)
{
    changePlaybackSpeed(speed, true);
}

void PlayerController::clearSyncPlaybackSpeed()
{
    changePlaybackSpeed(1.0, false, true);
}

void PlayerController::changePlaybackSpeed(double speed, bool syncOverride, bool clearSyncOverride)
{
    if (!std::isfinite(speed))
        return;

    const double oldUserSpeed = m_playbackSpeed;
    const double oldEffectiveSpeed = effectivePlaybackSpeed();

    if (clearSyncOverride) {
        if (!m_syncPlaybackSpeedActive)
            return;
        m_syncPlaybackSpeedActive = false;
        m_syncPlaybackSpeed = 1.0;
    } else if (syncOverride) {
        const double clampedSpeed = qBound(0.2, std::round(speed * 1000.0) / 1000.0, 2.0);
        if (m_syncPlaybackSpeedActive && qFuzzyCompare(m_syncPlaybackSpeed, clampedSpeed))
            return;
        m_syncPlaybackSpeedActive = true;
        m_syncPlaybackSpeed = clampedSpeed;
    } else {
        const double clampedSpeed = qBound(0.25, std::round(speed * 1000.0) / 1000.0, 4.0);
        if (qFuzzyCompare(m_playbackSpeed, clampedSpeed))
            return;
        m_playbackSpeed = clampedSpeed;
    }

    const double newEffectiveSpeed = effectivePlaybackSpeed();
    const bool effectiveChanged = !qFuzzyCompare(oldEffectiveSpeed, newEffectiveSpeed);
    qInfo() << "player: playback speed changed"
            << "user=" << m_playbackSpeed << "effective=" << newEffectiveSpeed
            << "syncOverride=" << m_syncPlaybackSpeedActive;

    if (effectiveChanged) {
        if (auto *handle = m_mpvLifecycle.handle()) {
            applyMpvRuntimeOption(MpvRuntimeOption::PlaybackSpeed, MpvOptionApplyMode::Runtime, handle);
        } else {
            discardPreparedMpvForOptionChange("playback speed change");
        }
        emit effectivePlaybackSpeedChanged();
    }
    if (!qFuzzyCompare(oldUserSpeed, m_playbackSpeed))
        emit playbackSpeedChanged();
}

void PlayerController::setSubtitlePreferences(const SubtitlePreferences& preferences)
{
    if (m_subtitlePreferences == preferences)
        return;

    const SubtitlePreferences previousPreferences = m_subtitlePreferences;
    const bool preserveTrackSelection = previousPreferences.language == preferences.language
        && previousPreferences.mode == preferences.mode && previousPreferences.audioMode == preferences.audioMode
        && previousPreferences.audioLanguage == preferences.audioLanguage;
    m_subtitlePreferences = preferences;
    qInfo() << "player: track preferences changed"
            << "subtitleMode=" << preferences.mode << "subtitleLanguage=" << preferences.language
            << "audioMode=" << preferences.audioMode << "audioLanguage=" << preferences.audioLanguage
            << "styling=" << preferences.styling << "colorOverride=" << preferences.overrideTextColor
            << "geometryOverride=" << preferences.alwaysOverridePositionAndSize
            << "allowBlackBars=" << preferences.allowSubtitlesInBlackBars
            << "sharpness=" << preferences.bitmapSharpnessPercent << "subPos=" << preferences.verticalPosition
            << "subScale=" << preferences.scalePercent;
    if (auto *handle = m_mpvLifecycle.handle()) {
        applyMpvSubtitleOptions(MpvOptionApplyMode::Runtime, handle, preserveTrackSelection,
            preserveTrackSelection || usesUserMpvConfig() ? &previousPreferences : nullptr);
    } else {
        discardPreparedMpvForOptionChange("subtitle preferences change");
    }
}

void PlayerController::previewSubtitlePreferences(const SubtitlePreferences& preferences)
{
    mpv_handle *handle = m_mpvLifecycle.handle();
    if (!handle || preferences == m_subtitlePreferences)
        return;

    const auto current
        = MpvOptionProfile::subtitleOptions(m_subtitlePreferences, m_tracks.subtitlesEnabled(), m_hdrPlayback);
    const auto preview = MpvOptionProfile::subtitleOptions(preferences, m_tracks.subtitlesEnabled(), m_hdrPlayback);
    for (const MpvOption& option : preview) {
        const auto previous = std::find_if(current.begin(), current.end(),
            [&option](const MpvOption& candidate) { return candidate.name == option.name; });
        if (previous != current.end() && previous->value == option.value)
            continue;

        const char *value = option.value.constData();
        const int error = mpv_set_property_async(handle, 0, option.name.constData(), MPV_FORMAT_STRING, &value);
        if (error < 0 && error != MPV_ERROR_OPTION_NOT_FOUND) {
            qWarning() << "player: failed to preview subtitle option" << option.name << mpv_error_string(error);
        }
    }
}

void PlayerController::setDemuxerBudget(const QByteArray& maxBytes, const QByteArray& maxBackBytes)
{
    bool changed = false;
    if (!maxBytes.isEmpty())
        m_automaticDemuxerMaxBytes = maxBytes;
    const QByteArray effectiveMaxBytes
        = m_forwardCacheSizeMiB > 0 ? QByteArray::number(m_forwardCacheSizeMiB) + 'M' : m_automaticDemuxerMaxBytes;
    if (!effectiveMaxBytes.isEmpty() && m_demuxerMaxBytes != effectiveMaxBytes) {
        m_demuxerMaxBytes = effectiveMaxBytes;
        changed = true;
    }
    if (!maxBackBytes.isEmpty() && m_demuxerMaxBackBytes != maxBackBytes) {
        m_demuxerMaxBackBytes = maxBackBytes;
        changed = true;
    }
    if (changed)
        discardPreparedMpvForOptionChange("demuxer budget change");
}

void PlayerController::setForwardCacheSizeMiB(int sizeMiB)
{
    sizeMiB = std::clamp(sizeMiB, 16, 4096);
    if (sizeMiB == m_forwardCacheSizeMiB)
        return;

    m_forwardCacheSizeMiB = sizeMiB;
    const QByteArray effectiveMaxBytes = QByteArray::number(sizeMiB) + 'M';
    if (effectiveMaxBytes == m_demuxerMaxBytes)
        return;

    m_demuxerMaxBytes = effectiveMaxBytes;
    qInfo() << "player: forward cache size" << QString::number(sizeMiB) + QStringLiteral(" MiB");
    discardPreparedMpvForOptionChange("forward cache size change");
}
void PlayerController::setMpvConfigPolicy(const MpvConfigPolicy& policy)
{
    if (!policy.valid || policy == m_mpvConfigPolicy)
        return;
    m_mpvConfigPolicy = policy;
    qInfo() << "player: mpv configuration policy changed; applies on next playback";
    discardPreparedMpvForOptionChange("mpv config policy change");
}

void PlayerController::startProgressReporting()
{
    Diagnostics::logEvent(QStringLiteral("player"), QStringLiteral("progress_reporting_start"),
        { { QStringLiteral("itemId"), m_session.itemId } });
    if (m_progressTimer.isActive())
        return;
    m_progressTimer.start();

    updateReportedStreamSelection(false);
    m_reporter.start(m_session, effectivePlaybackSpeed(), m_volume.load(), m_muted.load());
}

void PlayerController::stopProgressReporting(bool failed, bool completed)
{
    Diagnostics::Phase phase(QStringLiteral("player"), QStringLiteral("stop_progress_reporting"),
        { { QStringLiteral("failed"), failed }, { QStringLiteral("completed"), completed } });
    if (!m_sessionActive && !m_progressTimer.isActive()) {
        qInfo() << "player: stopProgressReporting skipped sessionActive=" << m_sessionActive;
        return;
    }

    const bool wasVisible = m_visible;
    const bool wasSessionActive = m_sessionActive;
    qInfo() << "player: stopProgressReporting sessionActive=" << m_sessionActive << "visible=" << m_visible
            << "failed=" << failed << "completed=" << completed;
    m_progressTimer.stop();
    m_uiPositionTimer.stop();
    m_seekWatchdogTimer.stop();

    const auto session = m_session;
    const qint64 positionTicks
        = completed && session.runtimeTicks > 0 ? session.runtimeTicks : secondsToTicks(m_positionTracker.position());
    m_reporter.stop(positionTicks, failed, effectivePlaybackSpeed());

    resetPlaybackUiState();
    m_window->clearOverlay();
    emit positionChanged();
    emit playbackStateChanged();
    emit tracksChanged();
    emit segmentsChanged();
    emit trickplayChanged();
    if (wasVisible != m_visible)
        emit visibleChanged();
    if (wasSessionActive != m_sessionActive)
        emit sessionActiveChanged();
    emit playbackStopped(session.itemId, positionTicks, completed);
}

void PlayerController::setDirectVideoOutput(bool direct)
{
    if (m_directVideoOutput == direct)
        return;
    m_directVideoOutput = direct;
    qInfo() << "player: video output" << (direct ? "direct" : "enhanced");
}

void PlayerController::setRenderQuality(MpvOptionProfile::RenderQuality quality)
{
    if (m_renderQuality == quality)
        return;
    m_renderQuality = quality;
    qInfo() << "player: render quality" << MpvOptionProfile::renderQualityName(quality).constData();
}

void PlayerController::resetRenderStrain()
{
    m_renderStrainTimer.stop();
    m_renderStrainReported = false;
    m_decoderDroppedFrames = 0;
    m_outputDroppedFrames = 0;
    m_delayedFrames = 0;
    m_outputFps = 0.0;
    m_containerFps = 0.0;
}

void PlayerController::resetPlaybackUiState()
{
    releaseMpvKeys();
    m_visible = false;
    m_sessionActive = false;
    m_fileLoaded = false;
    m_seekDispatchReady = false;
    if (m_hdrPlayback) {
        m_hdrPlayback = false;
        emit hdrPlaybackChanged();
    }
    m_paused = false;
    m_buffering = false;
    m_bufferingPercent = 0;
    m_seeking = false;
    m_pendingSeek = false;
    m_pendingSeekFlags.clear();
    m_positionTracker.clear();
    m_debugOsdVisible = false;
    resetRenderStrain();
    m_timeline.clear();
    rebuildTrickplaySheetUrls();
    m_statusText = QStringLiteral("Ready");
    m_mediaKind = QStringLiteral("none");
    if (m_tracks.clearChapters()) {
        emit chaptersChanged();
    }
}

bool PlayerController::mpvCommand(QByteArrayList command)
{
    auto *handle = m_mpvLifecycle.handle();
    if (!handle) {
        qInfo() << "player: mpv command dropped (no handle):" << command;
        return false;
    }

    QList<const char *> argv;
    argv.reserve(command.size() + 1);
    for (const QByteArray& arg : std::as_const(command))
        argv.append(arg.constData());
    argv.append(nullptr);

    const int error = mpv_command_async(handle, 0, argv.data());
    if (error < 0) {
        qWarning() << "player: mpv_command_async failed" << command << "error=" << error << mpv_error_string(error);
        return false;
    }
    return true;
}

QByteArrayList PlayerController::buildSeekCommand(double targetSeconds, const QByteArray& flags) const
{
    return { QByteArrayLiteral("no-osd"), QByteArrayLiteral("seek"), QByteArray::number(targetSeconds, 'f', 3), flags };
}

bool PlayerController::beginSeekCommand(double targetSeconds, const QByteArray& flags)
{
    if (!m_sessionActive)
        return false;

    const double clampedTarget = clampedPosition(targetSeconds);
    m_seeking = true;
    // A seek waiting for its first dispatch is re-aimed rather than counted
    // again: only one command is going out, so only one restart comes back.
    if (m_pendingSeek)
        m_positionTracker.replaceSeek(clampedTarget);
    else
        m_positionTracker.beginSeek(clampedTarget);
    // The bar follows the gesture at once, and lands back on mpv's own
    // position when the seek settles.
    emit positionChanged();
    notifyPlaybackStateChanged();

    if (!m_seekDispatchReady) {
        m_pendingSeek = true;
        m_pendingSeekTargetSeconds = clampedTarget;
        m_pendingSeekFlags = flags;
        qInfo() << "player: deferring seek until initial playback restart target=" << clampedTarget
                << "flags=" << flags;
        return true;
    }

    m_pendingSeek = false;
    m_pendingSeekFlags.clear();
    m_seekWatchdogTimer.start();
    if (mpvCommand(buildSeekCommand(clampedTarget, flags)))
        return true;

    m_seeking = false;
    m_seekWatchdogTimer.stop();
    m_positionTracker.abandonSeeks();
    notifyPlaybackStateChanged();
    return false;
}

bool PlayerController::beginRelativeSeekCommand(double deltaSeconds)
{
    if (!std::isfinite(deltaSeconds) || deltaSeconds == 0.0)
        return false;

    const double optimisticTarget = clampedPosition(seekAnchorPosition() + deltaSeconds);
    qInfo() << "player: relative keyframe seek" << deltaSeconds << "absoluteTarget=" << optimisticTarget;
    return beginSeekCommand(optimisticTarget, QByteArrayLiteral("absolute+keyframes"));
}

void PlayerController::flushPendingSeek()
{
    if (!m_pendingSeek || !m_sessionActive || !m_seekDispatchReady)
        return;

    const double target = m_pendingSeekTargetSeconds;
    const QByteArray flags = m_pendingSeekFlags;
    m_pendingSeek = false;
    m_pendingSeekFlags.clear();
    m_seekWatchdogTimer.start();
    qInfo() << "player: dispatching deferred seek target=" << target << "flags=" << flags;
    if (mpvCommand(buildSeekCommand(target, flags)))
        return;

    m_seeking = false;
    m_seekWatchdogTimer.stop();
    m_positionTracker.abandonSeeks();
    notifyPlaybackStateChanged();
}

void PlayerController::handleMpvEvent(mpv_event *event)
{
    if (!event)
        return;

    switch (event->event_id) {
    case MPV_EVENT_FILE_LOADED:
        m_mpvLifecycle.completeFileLoad();
        QMetaObject::invokeMethod(this, [this]() {
            qInfo() << "player: file loaded";
            m_fileLoaded = true;
            // Only video can strain the renderer, and only once per playback:
            // a step down rebuilds the core, which lands back here.
            if (!m_renderStrainReported && m_mediaKind == QStringLiteral("video"))
                m_renderStrainTimer.start();
            // Every play request builds a fresh mpv core, which starts with the
            // stats overlay off. A restart the viewer did not ask for, like a
            // quality change, should not take their stats away with it.
            if (m_debugOsdVisible && m_mediaKind == QStringLiteral("video"))
                mpvCommand({ QByteArrayLiteral("script-binding"), QByteArrayLiteral("stats/display-stats-toggle") });
            notifyPlaybackStateChanged();
            startProgressReporting();
        });
        break;
    case MPV_EVENT_PLAYBACK_RESTART:
        QMetaObject::invokeMethod(this, [this]() {
            qInfo() << "player: playback restart";
            const bool hadPendingSeek = m_pendingSeek;
            m_seekDispatchReady = true;
            if (hadPendingSeek) {
                // The seek has not been issued yet, so this restart belongs to
                // the file starting, not to it.
                flushPendingSeek();
            } else {
                // This is how a seek is known to have landed: from here mpv's
                // clock is the position again, so ask for it rather than
                // waiting out a tick of its own.
                m_positionTracker.settleSeek();
                if (!m_positionTracker.seekInFlight()) {
                    m_seeking = false;
                    m_seekWatchdogTimer.stop();
                    requestMpvPositionRefresh("seek settled");
                } else {
                    m_seekWatchdogTimer.start();
                }
            }
            notifyPlaybackStateChanged();
        });
        break;
    case MPV_EVENT_GET_PROPERTY_REPLY: {

        if (event->reply_userdata != kTimePosRefreshReply)
            break;

        auto *property = static_cast<mpv_event_property *>(event->data);
        if (!property || !property->data || property->format != MPV_FORMAT_DOUBLE)
            break;

        const double seconds = *static_cast<double *>(property->data);
        QMetaObject::invokeMethod(this, [this, seconds]() { setPositionSeconds(seconds); });
        break;
    }
    case MPV_EVENT_PROPERTY_CHANGE: {
        auto *property = static_cast<mpv_event_property *>(event->data);
        if (!property || !property->data)
            break;

        if (strcmp(property->name, "pause") == 0 && property->format == MPV_FORMAT_FLAG) {
            const bool paused = *static_cast<int *>(property->data);
            QMetaObject::invokeMethod(this, [this, paused]() {
                if (m_paused != paused)
                    qInfo() << "player: pause state changed" << paused;
                m_paused = paused;
                if (!m_paused)
                    requestMpvPositionRefresh("unpause");
                notifyPlaybackStateChanged();
            });
        } else if (strcmp(property->name, "paused-for-cache") == 0 && property->format == MPV_FORMAT_FLAG) {
            const bool buffering = *static_cast<int *>(property->data);
            QMetaObject::invokeMethod(this, [this, buffering]() {
                m_buffering = buffering;
                if (!buffering)
                    m_bufferingPercent = 0;
                notifyPlaybackStateChanged();
            });
        } else if (strcmp(property->name, "cache-buffering-state") == 0 && property->format == MPV_FORMAT_INT64) {
            const auto percent = static_cast<int>(*static_cast<int64_t *>(property->data));
            QMetaObject::invokeMethod(this, [this, percent]() {
                m_bufferingPercent = percent;
                notifyPlaybackStateChanged();
            });
        } else if (strcmp(property->name, "vo-delayed-frame-count") == 0 && property->format == MPV_FORMAT_INT64) {
            const qint64 count = *static_cast<int64_t *>(property->data);
            QMetaObject::invokeMethod(this, [this, count]() {
                if (m_delayedFrames == count)
                    return;
                m_delayedFrames = count;
                emit performanceStatsChanged();
            });
        } else if ((strcmp(property->name, "estimated-vf-fps") == 0 || strcmp(property->name, "container-fps") == 0)
            && property->format == MPV_FORMAT_DOUBLE) {
            const bool container = strcmp(property->name, "container-fps") == 0;
            const double fps = *static_cast<double *>(property->data);
            QMetaObject::invokeMethod(this, [this, container, fps]() {
                double& current = container ? m_containerFps : m_outputFps;
                if (qFuzzyCompare(current, fps))
                    return;
                current = fps;
                emit performanceStatsChanged();
            });
        } else if ((strcmp(property->name, "decoder-frame-drop-count") == 0
                       || strcmp(property->name, "frame-drop-count") == 0)
            && property->format == MPV_FORMAT_INT64) {
            const bool decoder = strcmp(property->name, "decoder-frame-drop-count") == 0;
            const qint64 count = *static_cast<int64_t *>(property->data);
            QMetaObject::invokeMethod(this, [this, decoder, count]() {
                qint64& current = decoder ? m_decoderDroppedFrames : m_outputDroppedFrames;
                if (current == count)
                    return;
                current = count;
                emit performanceStatsChanged();
            });
        } else if (strcmp(property->name, "seeking") == 0 && property->format == MPV_FORMAT_FLAG) {
            const bool seeking = *static_cast<int *>(property->data);
            QMetaObject::invokeMethod(this, [this, seeking]() {
                m_seeking = seeking;
                // mpv can clear this before it restarts playback. The seek is
                // not settled until the restart, so the watchdog stays armed.
                if (m_seeking || m_positionTracker.seekInFlight())
                    m_seekWatchdogTimer.start();
                else
                    m_seekWatchdogTimer.stop();
                notifyPlaybackStateChanged();
            });
        } else if (strcmp(property->name, "time-pos") == 0 && property->format == MPV_FORMAT_DOUBLE) {
            const double seconds = *static_cast<double *>(property->data);
            QMetaObject::invokeMethod(this, [this, seconds]() { setPositionSeconds(seconds); });
        } else if (strcmp(property->name, "duration") == 0 && property->format == MPV_FORMAT_DOUBLE) {
            const double seconds = *static_cast<double *>(property->data);
            QMetaObject::invokeMethod(this, [this, seconds]() {
                m_positionTracker.setDuration(seconds);
                if (m_timeline.updatePosition(m_positionTracker.position()))
                    emit segmentsChanged();
                emit positionChanged();
            });
        } else if (strcmp(property->name, "volume") == 0 && property->format == MPV_FORMAT_DOUBLE) {
            const auto volume = static_cast<int>(std::round(*static_cast<double *>(property->data)));
            QMetaObject::invokeMethod(this, [this, volume]() {
                const int clampedVolume = qBound(0, volume, 100);
                if (m_volume.load() == clampedVolume)
                    return;
                m_volume = clampedVolume;
                emit volumeChanged();
            });
        } else if (strcmp(property->name, "fullscreen") == 0 && property->format == MPV_FORMAT_FLAG) {
            const bool fullscreen = *static_cast<int *>(property->data);
            QMetaObject::invokeMethod(this, [this, fullscreen]() {
                if (m_window && m_window->fullScreen() != fullscreen)
                    m_window->toggleFullScreen();
            });
        } else if (strcmp(property->name, "speed") == 0 && property->format == MPV_FORMAT_DOUBLE) {
            const double speed = *static_cast<double *>(property->data);
            QMetaObject::invokeMethod(this, [this, speed]() {
                if (!m_syncPlaybackSpeedActive && !qFuzzyCompare(m_playbackSpeed, speed)) {
                    m_playbackSpeed = speed;
                    emit playbackSpeedChanged();
                    emit effectivePlaybackSpeedChanged();
                }
            });
        } else if (strcmp(property->name, "mute") == 0 && property->format == MPV_FORMAT_FLAG) {
            const bool muted = *static_cast<int *>(property->data);
            QMetaObject::invokeMethod(this, [this, muted]() {
                if (m_muted.exchange(muted) != muted)
                    emit volumeChanged();
            });
        } else if ((strcmp(property->name, "current-vo") == 0 || strcmp(property->name, "current-gpu-context") == 0
                       || strcmp(property->name, "video-codec") == 0
                       || strcmp(property->name, "video-dec-params/pixelformat") == 0)
            && property->format == MPV_FORMAT_STRING) {
            const auto value = *static_cast<char **>(property->data);
            qInfo() << "player: output diagnostic" << property->name << (value ? value : "unavailable");
        } else if (strcmp(property->name, "hwdec-current") == 0 && property->format == MPV_FORMAT_STRING) {
            const auto *decoder = static_cast<char **>(property->data);
            const QByteArray decoderName(decoder && *decoder ? *decoder : "");
            QMetaObject::invokeMethod(this, [decoderName]() {
                qInfo() << "player: hardware decoder"
                        << (decoderName.isEmpty() ? QByteArrayLiteral("none") : decoderName);
            });
        } else if ((strcmp(property->name, "dwidth") == 0 || strcmp(property->name, "dheight") == 0)
            && property->format == MPV_FORMAT_INT64) {
            const bool isWidth = strcmp(property->name, "dwidth") == 0;
            const auto value = static_cast<int>(*static_cast<int64_t *>(property->data));
            QMetaObject::invokeMethod(this, [this, isWidth, value]() {
                (isWidth ? m_videoWidth : m_videoHeight) = value;
                if (m_videoWidth > 0 && m_videoHeight > 0)
                    platformVideoSizeChanged(m_videoWidth, m_videoHeight);
            });
        } else if (strcmp(property->name, "video-params/transfer") == 0 && property->format == MPV_FORMAT_STRING) {
            const auto *transferValue = static_cast<char **>(property->data);
            const QByteArray transfer(transferValue && *transferValue ? *transferValue : "");
            QMetaObject::invokeMethod(this, [this, transfer]() {
                m_hdrInput = MpvOptionProfile::isHdrTransfer(transfer);
                if (m_starfishVideoOutput)
                    updateHdrOutput(true);
                qInfo() << "player: input video transfer" << transfer << "HDR=" << m_hdrInput;
            });
        } else if (strcmp(property->name, "video-target-params/transfer") == 0
            && property->format == MPV_FORMAT_STRING) {
            const auto *transferValue = static_cast<char **>(property->data);
            const QByteArray transfer(transferValue && *transferValue ? *transferValue : "");
            QMetaObject::invokeMethod(this, [this, transfer]() {
                m_targetTransfer = transfer;
                if (!m_starfishVideoOutput)
                    updateHdrOutput(true);
                qInfo() << "player: output video transfer" << transfer
                        << "HDR=" << MpvOptionProfile::isHdrTransfer(transfer);
            });
        } else if (strcmp(property->name, "track-list") == 0 && property->format == MPV_FORMAT_NODE) {
            const auto *node = static_cast<mpv_node *>(property->data);
            const ParsedPlaybackTracks tracks = PlaybackTrackParser::parseTracks(node);
            QMetaObject::invokeMethod(this, [this, tracks]() {
                m_tracks.applyParsedTracks(tracks);
                if (m_restoreStreamSelection) {
                    const int audioStreamIndex = m_session.audioStreamIndex;
                    const int subtitleStreamIndex = m_session.subtitleStreamIndex;
                    const int audioUiIndex = uiTrackIndexForStream(QStringLiteral("Audio"), audioStreamIndex, 0);
                    const int subtitleUiIndex = subtitleStreamIndex < 0
                        ? 0
                        : uiTrackIndexForStream(QStringLiteral("Subtitle"), subtitleStreamIndex, 1);
                    const bool tracksPending = (audioUiIndex >= 0 && !m_tracks.audioCommand(audioUiIndex))
                        || (subtitleUiIndex >= 0 && !m_tracks.subtitleCommand(subtitleUiIndex));
                    if (!tracksPending) {
                        m_restoreStreamSelection = false;
                        if (audioUiIndex >= 0)
                            selectAudio(audioUiIndex);
                        if (subtitleUiIndex >= 0)
                            selectSubtitle(subtitleUiIndex);
                    }
                }
                if (!m_restoreStreamSelection)
                    updateReportedStreamSelection(true);
                qInfo() << "player: subtitle tracks" << tracks.subtitleLabels << "selected"
                        << tracks.selectedSubtitleIndex << "audio tracks" << tracks.audioLabels << "selected"
                        << tracks.selectedAudioIndex;
                emit tracksChanged();
            });
        } else if (strcmp(property->name, "chapter-list") == 0 && property->format == MPV_FORMAT_NODE) {
            const auto *node = static_cast<mpv_node *>(property->data);
            const QVariantList chapters = PlaybackTrackParser::parseChapters(node);
            QMetaObject::invokeMethod(this, [this, chapters]() {
                m_tracks.setChapters(chapters);
                qInfo() << "player: chapters" << chapters.size();
                emit chaptersChanged();
            });
        } else if (strcmp(property->name, "chapter") == 0 && property->format == MPV_FORMAT_INT64) {
            const int chapter = static_cast<int>(*static_cast<int64_t *>(property->data));
            QMetaObject::invokeMethod(this, [this, chapter]() {
                if (m_tracks.setCurrentChapter(chapter))
                    emit chaptersChanged();
            });
        }
        break;
    }
    case MPV_EVENT_END_FILE: {
        auto *endFile = static_cast<mpv_event_end_file *>(event->data);
        const bool failed = endFile && endFile->error < 0;
        const int endFileReason = endFile ? endFile->reason : -1;
        const int endFileError = endFile ? endFile->error : 0;
        const bool failedBeforeLoad = failed && !m_fileLoaded;
        // A fresh mpv core is created for every play request, so an END_FILE
        // while loading belongs to this request. Clear the pending marker on
        // both success and failure; otherwise a failed manifest stays stuck in
        // the preparing state forever.
        m_mpvLifecycle.cancelFileLoad();
        const bool completed = !failed && endFileReason == MPV_END_FILE_REASON_EOF;
        QMetaObject::invokeMethod(this, [this, failed, failedBeforeLoad, completed, endFileReason, endFileError]() {
            qInfo() << "player: end file (main thread) failed=" << failed << "completed=" << completed
                    << "sessionActive=" << m_sessionActive << "reason=" << endFileReason
                    << endFileReasonName(endFileReason) << "error=" << endFileError
                    << (endFileError < 0 ? mpv_error_string(endFileError) : "");
            if (failed) {
                m_errorText
                    = QStringLiteral("Playback failed: %1").arg(QString::fromUtf8(mpv_error_string(endFileError)));
                // Which stream mpv could not open is the first thing anyone
                // reading this log wants to know.
                qWarning() << "player: playback failed method=" << m_session.playMethod
                           << "url=" << sanitizedDiagnosticUrl(m_session.url);
            }
            const QString failedItemId = m_session.itemId;
            const qint64 failedPositionTicks = secondsToTicks(m_positionTracker.position());
            const QString failureMessage = m_errorText;
            const int audioStreamIndex = m_session.audioStreamIndex;
            const int subtitleStreamIndex = m_session.subtitleStreamIndex;
            const bool retryableCodecFailure
                = PlaybackFailurePolicy::isRetryableCodecFailure(m_session.playMethod, failedBeforeLoad, endFileError);
            stopProgressReporting(failed, completed);
            if (failedBeforeLoad) {
                emit playbackLoadFailed(failedItemId, failedPositionTicks, failureMessage, retryableCodecFailure,
                    audioStreamIndex, subtitleStreamIndex);
            }
            scheduleMpvTeardown();
        });
        break;
    }
    case MPV_EVENT_SHUTDOWN:
        m_mpvLifecycle.requestEventLoopStop();
        QMetaObject::invokeMethod(this, [this]() {
            qInfo() << "player: mpv shutdown";
            if (m_sessionActive)
                stopProgressReporting(false);
            scheduleMpvTeardown();
        });
        break;
    case MPV_EVENT_LOG_MESSAGE: {
        const auto *message = static_cast<mpv_event_log_message *>(event->data);
        if (!message)
            break;
        QByteArray text = QByteArray(message->text).trimmed();
        if (text.isEmpty())
            break;
        qInfo().nospace() << "mpv/" << message->prefix << " [" << message->level << "] " << text.constData();
        break;
    }
    default:
        break;
    }
}

void PlayerController::updatePlaybackStatusText()
{
    if (m_seeking) {
        m_statusText = QStringLiteral("Seeking…");
        return;
    }

    if (m_buffering) {
        if (m_bufferingPercent > 0)
            m_statusText = QStringLiteral("Buffering %1%").arg(m_bufferingPercent);
        else
            m_statusText = QStringLiteral("Buffering…");
        return;
    }

    m_statusText = m_paused ? QStringLiteral("Paused") : QStringLiteral("Playing");
}

void PlayerController::notifyPlaybackStateChanged()
{
    updatePlaybackStatusText();
    emit playbackStateChanged();
}

double PlayerController::clampedPosition(double seconds) const
{
    return m_positionTracker.clamp(seconds);
}

double PlayerController::seekAnchorPosition()
{
    // While a seek is in flight the tracker already reads as its target, so a
    // gesture chained onto one compounds instead of starting again from the
    // position mpv has not left yet.
    return m_positionTracker.position();
}

void PlayerController::requestMpvPositionRefresh(const char *reason)
{
    auto *handle = m_mpvLifecycle.handle();
    if (!m_sessionActive || !handle)
        return;

    const int error = mpv_get_property_async(handle, kTimePosRefreshReply, "time-pos", MPV_FORMAT_DOUBLE);
    if (error < 0)
        qWarning() << "player: async time-pos refresh failed" << (reason ? reason : "unknown")
                   << mpv_error_string(error);
}

void PlayerController::setPositionSeconds(double seconds, bool notifySegments)
{
    // Tearing down mpv leaves its last position events queued for the main
    // thread, so they land after play() has reset the tracker for the new item
    // -- and they are about the item that just ended. Until the new file is
    // loaded the tracker already holds the requested start position, and
    // nothing mpv says about the old one is worth hearing.
    if (!m_fileLoaded)
        return;

    if (!m_positionTracker.update(seconds))
        return;

    const bool segmentChanged = m_timeline.updatePosition(m_positionTracker.position());

    emit positionChanged();
    if (notifySegments && segmentChanged)
        emit segmentsChanged();
}

QString PlayerController::activeSegmentType() const
{
    return m_timeline.activeSegmentType();
}
double PlayerController::activeSegmentEndSeconds() const
{
    return m_timeline.activeSegmentEndSeconds();
}
bool PlayerController::trickplayAvailable() const
{
    return m_timeline.trickplayAvailable();
}

QStringList PlayerController::trickplaySheetUrls() const
{
    return m_trickplaySheetUrls;
}

void PlayerController::rebuildTrickplaySheetUrls()
{
    m_trickplaySheetUrls.clear();
    if (!trickplayAvailable() || !m_api)
        return;

    const int sheetCount = m_timeline.trickplaySheetCount();
    m_trickplaySheetUrls.reserve(sheetCount);
    for (int i = 0; i < sheetCount; ++i) {
        m_trickplaySheetUrls.push_back(m_api->trickplayTileUrl(m_session.itemId, m_timeline.trickplayWidth(), i));
    }
}

void PlayerController::skipActiveSegment()
{
    if (activeSegmentType().isEmpty() || activeSegmentEndSeconds() <= 0.0)
        return;
    seek(activeSegmentEndSeconds());
}

QVariantMap PlayerController::trickplayForSeconds(double seconds) const
{
    // Returns { url, width, height, offsetX, offsetY, available } so QML can
    // paint a single tile sprite from a positioned BorderImage / clipped Image.
    QVariantMap result;
    if (!trickplayAvailable() || !m_api) {
        result.insert(QStringLiteral("available"), false);
        return result;
    }
    const PlaybackTimeline::TrickplayFrame frame = m_timeline.trickplayFrameAt(seconds);
    if (!frame.available) {
        result.insert(QStringLiteral("available"), false);
        return result;
    }
    result.insert(QStringLiteral("available"), true);
    result.insert(QStringLiteral("url"),
        m_api->trickplayTileUrl(m_session.itemId, m_timeline.trickplayWidth(), frame.sheetIndex));
    result.insert(QStringLiteral("width"), frame.width);
    result.insert(QStringLiteral("height"), frame.height);
    result.insert(QStringLiteral("offsetX"), frame.offsetX);
    result.insert(QStringLiteral("offsetY"), frame.offsetY);
    result.insert(QStringLiteral("sheetWidth"), frame.sheetWidth);
    result.insert(QStringLiteral("sheetHeight"), frame.sheetHeight);
    return result;
}

} // namespace JellyfinNative
