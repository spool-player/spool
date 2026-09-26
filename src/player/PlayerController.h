#pragma once

#include "../media/MediaTypes.h"
#include "../platform/MpvConfigPolicy.h"
#include "MpvLifecycle.h"
#include "MpvOptionProfile.h"
#include "PlaybackPositionTracker.h"
#include "PlaybackReporter.h"
#include "PlaybackTimeline.h"
#include "PlaybackTrackState.h"
#include "RenderTargetProfile.h"

#include <QByteArray>
#include <QByteArrayList>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariant>

#include <atomic>
#include <vector>

struct mpv_handle;

namespace Spool {

class NativeAppWindow;
class PlaybackSource;
class TlsTrustController;

class PlayerController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool visible READ visible NOTIFY visibleChanged)
    Q_PROPERTY(bool sessionActive READ sessionActive NOTIFY sessionActiveChanged)
    Q_PROPERTY(bool fileLoaded READ fileLoaded NOTIFY playbackStateChanged)
    Q_PROPERTY(bool hdrPlayback READ hdrPlayback NOTIFY hdrPlaybackChanged)
    Q_PROPERTY(QString mediaKind READ mediaKind NOTIFY playbackStateChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY playbackStateChanged)
    Q_PROPERTY(QString title READ title NOTIFY playbackStateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY playbackStateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY playbackStateChanged)
    Q_PROPERTY(bool buffering READ buffering NOTIFY playbackStateChanged)
    Q_PROPERTY(int bufferingPercent READ bufferingPercent NOTIFY playbackStateChanged)
    Q_PROPERTY(bool seeking READ seeking NOTIFY playbackStateChanged)
    Q_PROPERTY(bool debugOsdVisible READ debugOsdVisible NOTIFY playbackStateChanged)
    Q_PROPERTY(bool embeddedVideoOutput READ embeddedVideoOutput NOTIFY playbackStateChanged)
    Q_PROPERTY(qint64 decoderDroppedFrames READ decoderDroppedFrames NOTIFY performanceStatsChanged)
    Q_PROPERTY(qint64 outputDroppedFrames READ outputDroppedFrames NOTIFY performanceStatsChanged)
    Q_PROPERTY(qint64 delayedFrames READ delayedFrames NOTIFY performanceStatsChanged)
    Q_PROPERTY(double outputFps READ outputFps NOTIFY performanceStatsChanged)
    Q_PROPERTY(double containerFps READ containerFps NOTIFY performanceStatsChanged)
    Q_PROPERTY(bool subtitlesEnabled READ subtitlesEnabled NOTIFY tracksChanged)
    Q_PROPERTY(QStringList subtitleTracks READ subtitleTracks NOTIFY tracksChanged)
    Q_PROPERTY(int selectedSubtitleIndex READ selectedSubtitleIndex NOTIFY tracksChanged)
    Q_PROPERTY(QStringList audioTracks READ audioTracks NOTIFY tracksChanged)
    Q_PROPERTY(int selectedAudioIndex READ selectedAudioIndex NOTIFY tracksChanged)
    Q_PROPERTY(bool backAllowed READ backAllowed NOTIFY playbackStateChanged)
    Q_PROPERTY(double positionSeconds READ positionSeconds NOTIFY positionChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds NOTIFY positionChanged)
    Q_PROPERTY(QVariantList chapters READ chapters NOTIFY chaptersChanged)
    Q_PROPERTY(bool hasChapters READ hasChapters NOTIFY chaptersChanged)
    Q_PROPERTY(int currentChapter READ currentChapter NOTIFY chaptersChanged)
    Q_PROPERTY(bool nightModeEnabled READ nightModeEnabled WRITE setNightModeEnabled NOTIFY nightModeEnabledChanged)
    Q_PROPERTY(bool toneMappingVisualizationEnabled READ toneMappingVisualizationEnabled WRITE
            setToneMappingVisualizationEnabled NOTIFY toneMappingVisualizationEnabledChanged)
    Q_PROPERTY(int audioDelayMs READ audioDelayMs WRITE setAudioDelayMs NOTIFY audioDelayMsChanged)
    Q_PROPERTY(int fileAudioDelayMs READ fileAudioDelayMs WRITE setFileAudioDelayMs NOTIFY fileAudioDelayMsChanged)
    Q_PROPERTY(int effectiveAudioDelayMs READ effectiveAudioDelayMs NOTIFY effectiveAudioDelayMsChanged)
    Q_PROPERTY(int subtitleDelayMs READ subtitleDelayMs WRITE setSubtitleDelayMs NOTIFY subtitleDelayMsChanged)
    Q_PROPERTY(QString audioOutputMode READ audioOutputMode WRITE setAudioOutputMode NOTIFY audioOutputModeChanged)
    Q_PROPERTY(int volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY volumeChanged)
    Q_PROPERTY(double playbackSpeed READ playbackSpeed WRITE setPlaybackSpeed NOTIFY playbackSpeedChanged)
    Q_PROPERTY(double effectivePlaybackSpeed READ effectivePlaybackSpeed NOTIFY effectivePlaybackSpeedChanged)
    Q_PROPERTY(QString activeSegmentType READ activeSegmentType NOTIFY segmentsChanged)
    Q_PROPERTY(double activeSegmentEndSeconds READ activeSegmentEndSeconds NOTIFY segmentsChanged)
    Q_PROPERTY(bool trickplayAvailable READ trickplayAvailable NOTIFY trickplayChanged)
    Q_PROPERTY(QStringList trickplaySheetUrls READ trickplaySheetUrls NOTIFY trickplayChanged)

public:
    PlayerController(NativeAppWindow *window, PlaybackSource *api, TlsTrustController *tlsTrust,
        const QString& subtitleFontsPath, QObject *parent = nullptr);
    ~PlayerController() override;

    bool visible() const;
    bool sessionActive() const;
    bool fileLoaded() const;
    bool hdrPlayback() const;
    QString mediaKind() const;
    bool paused() const;
    QString title() const;
    QString statusText() const;
    QString errorText() const;
    bool buffering() const;
    int bufferingPercent() const;
    bool seeking() const;
    bool debugOsdVisible() const;
    bool embeddedVideoOutput() const;
    qint64 decoderDroppedFrames() const;
    qint64 outputDroppedFrames() const;
    qint64 delayedFrames() const
    {
        return m_delayedFrames;
    }
    double outputFps() const
    {
        return m_outputFps;
    }
    double containerFps() const
    {
        return m_containerFps;
    }
    bool subtitlesEnabled() const;
    QStringList subtitleTracks() const;
    int selectedSubtitleIndex() const;
    QStringList audioTracks() const;
    int selectedAudioIndex() const;
    bool backAllowed() const;
    double positionSeconds() const;
    double estimatedPositionSeconds() const;
    double durationSeconds() const;
    QVariantList chapters() const;
    bool hasChapters() const;
    int currentChapter() const;
    bool nightModeEnabled() const;
    bool toneMappingVisualizationEnabled() const;
    int audioDelayMs() const;
    int fileAudioDelayMs() const;
    int effectiveAudioDelayMs() const;
    int subtitleDelayMs() const;
    QString audioOutputMode() const;
    int volume() const;
    bool muted() const;
    double playbackSpeed() const;
    double effectivePlaybackSpeed() const;
    QString activeSegmentType() const;
    double activeSegmentEndSeconds() const;
    bool trickplayAvailable() const;
    QStringList trickplaySheetUrls() const;
    Q_INVOKABLE void skipActiveSegment();
    Q_INVOKABLE QVariantMap trickplayForSeconds(double seconds) const;

    Q_INVOKABLE void play(const Spool::PlaybackSession& session, bool startPaused = false);
    void setMediaSegments(const QString& itemId, const std::vector<MediaSegment>& segments);
    Q_INVOKABLE void togglePause();
    Q_INVOKABLE bool forwardMpvKey(int key, int modifiers, const QString& text, bool pressed, bool repeat);
    Q_INVOKABLE void releaseMpvKeys();
    void setPaused(bool paused);
    Q_INVOKABLE void seekBack();
    Q_INVOKABLE void seekForward();
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE void previewSeekBy(double deltaSeconds);
    // Where a fresh seek gesture should start from: the target of a seek still
    // in flight, so chained gestures compound instead of cancelling out.
    Q_INVOKABLE double seekAnchorSeconds();
    // mpv's own stats page as text, for a window that has to paint it itself.
    Q_INVOKABLE QString mpvStatsPage();
    void prepareForBackground();
    void teardownForBackground();
    void resyncForForeground();
    Q_INVOKABLE void toggleDebugOsd();
    Q_INVOKABLE void toggleSubtitles();
    Q_INVOKABLE void cycleSubtitles();
    Q_INVOKABLE void enableSubtitles();
    Q_INVOKABLE void selectSubtitle(int index);
    Q_INVOKABLE void selectSubtitleStreamIndex(int streamIndex);
    Q_INVOKABLE void cycleAudio();
    Q_INVOKABLE void selectAudio(int index);
    Q_INVOKABLE void selectAudioStreamIndex(int streamIndex);
    Q_INVOKABLE void nextChapter();
    Q_INVOKABLE void previousChapter();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void stopWithReason(const QString& reason);
    Q_INVOKABLE void setNightModeEnabled(bool enabled);
    Q_INVOKABLE void setToneMappingVisualizationEnabled(bool enabled);
    Q_INVOKABLE void setAudioDelayMs(int delayMs);
    Q_INVOKABLE void setFileAudioDelayMs(int delayMs);
    Q_INVOKABLE void setSubtitleDelayMs(int delayMs);
    Q_INVOKABLE void setAudioOutputMode(const QString& mode);
    // Applies to the next thing that plays: the render options are set on a
    // fresh mpv core, and every play request builds one.
    void setRenderQuality(MpvOptionProfile::RenderQuality quality);
    void setHardwareDecoding(bool enabled)
    {
        m_hardwareDecoding = enabled;
    }
    void setSoftwareRenderer(const QByteArray& backend)
    {
        m_softwareRenderer = backend;
    }
    // Direct output keeps video off the Qt scene graph entirely. Applies to
    // the next thing that plays, like the quality profile.
    void setDirectVideoOutput(bool direct);
    bool directVideoOutput() const
    {
        return m_directVideoOutput;
    }
    MpvOptionProfile::RenderQuality renderQuality() const
    {
        return m_renderQuality;
    }
    Q_INVOKABLE void setVolume(int volume);
    Q_INVOKABLE void adjustVolume(int delta);
    Q_INVOKABLE void setMuted(bool muted);
    Q_INVOKABLE void toggleMuted();
    Q_INVOKABLE void setPlaybackSpeed(double speed);
    void setSyncPlaybackSpeed(double speed);
    void clearSyncPlaybackSpeed();
    void setSubtitlePreferences(const Spool::SubtitlePreferences& preferences);
    void previewSubtitlePreferences(const Spool::SubtitlePreferences& preferences);
    void setDemuxerBudget(const QByteArray& maxBytes, const QByteArray& maxBackBytes);
    void setForwardCacheSizeMiB(int sizeMiB);
    void setMpvConfigPolicy(const MpvConfigPolicy& policy);

signals:
    void visibleChanged();
    void sessionActiveChanged();
    void positionChanged();
    void hdrPlaybackChanged();
    void playbackStateChanged();
    void performanceStatsChanged();
    // Raised once per playback when the opening seconds drop more frames than
    // this device can be asked to absorb. Carries what was measured so the
    // decision about what to do with it can be made, and explained, elsewhere.
    void renderQualityStrained(qint64 droppedFrames);
    void tracksChanged();
    void segmentsChanged();
    void trickplayChanged();
    void chaptersChanged();
    void playbackStopped(const QString& itemId, qint64 positionTicks, bool completed);
    void playbackLoadFailed(const QString& itemId, qint64 positionTicks, const QString& message,
        bool retryableCodecFailure, int audioStreamIndex, int subtitleStreamIndex);
    void streamSelectionChanged(int audioStreamIndex, int subtitleStreamIndex);
    void nightModeEnabledChanged();
    void toneMappingVisualizationEnabledChanged();
    void audioDelayMsChanged();
    void fileAudioDelayMsChanged();
    void effectiveAudioDelayMsChanged();
    void subtitleDelayMsChanged();
    void audioOutputModeChanged();
    void volumeChanged();
    void playbackSpeedChanged();
    void effectivePlaybackSpeedChanged();

public:
    // Silence active playback before application services and the render
    // surface begin shutting down. Safe to call repeatedly.
    void prepareForShutdown();

    // Called from main on aboutToQuit so we tear down before the scene graph
    // stops accepting render jobs. Safe to call repeatedly. Pass async only
    // from the deferred post-stop path where blocking the GUI thread matters
    // more than deterministic completion.
    void teardownMpv(bool async = false);

private:
    QHash<int, QByteArray> m_mpvKeys;
    void logColorDiagnostics(mpv_handle *handle);
    bool usesUserMpvConfig() const;
    int uiTrackIndexForStream(const QString& type, int streamIndex, int firstUiIndex) const;
    int streamIndexForUiTrack(const QString& type, int uiIndex, int firstUiIndex) const;
    void updateReportedStreamSelection(bool sendProgress);
    enum class MpvOptionApplyMode {
        Initial,
        Runtime,
    };

    enum class MpvRuntimeOption {
        NightMode,
        ToneMappingVisualization,
        AudioDelay,
        SubtitleDelay,
        PlaybackSpeed,
    };

    bool ensureMpv(bool needsVideoSurface, bool embeddedVideo);
    void scheduleIdleMpvPreparation();
    void prepareIdleMpv();
    void destroyIdleMpv(const char *reason);
    mpv_handle *takeIdleMpvHandle();
    bool configureAndInitializeMpv(mpv_handle *handle, bool needsVideoSurface, bool embeddedVideo);
    void observeMpvProperties(mpv_handle *handle);
    void scheduleMpvTeardown();
    void handleMpvEvent(mpv_event *event);
    void startProgressReporting();
    void stopProgressReporting(bool failed = false, bool completed = false);
    bool mpvCommand(QByteArrayList command);
    bool beginSeekCommand(double targetSeconds, const QByteArray& flags);
    QByteArrayList buildSeekCommand(double targetSeconds, const QByteArray& flags) const;
    bool beginRelativeSeekCommand(double deltaSeconds);
    void stepChapter(int delta);
    void flushPendingSeek();
    void updatePlaybackStatusText();
    void notifyPlaybackStateChanged();
    void setPositionSeconds(double seconds, bool notifySegments = true);
    void requestMpvPositionRefresh(const char *reason);
    double clampedPosition(double seconds) const;
    double seekAnchorPosition();
    void resetPlaybackUiState();
    void resetRenderStrain();
    void rebuildTrickplaySheetUrls();
    bool applyMpvRuntimeOption(MpvRuntimeOption option, MpvOptionApplyMode mode, mpv_handle *handle);
    bool applyMpvSubtitleOptions(MpvOptionApplyMode mode, mpv_handle *handle, bool preserveTrackSelection = false,
        const SubtitlePreferences *previousPreferences = nullptr);
    bool applyMpvRuntimeOptions(MpvOptionApplyMode mode, mpv_handle *handle);
    void discardPreparedMpvForOptionChange(const char *reason);
    void handleVideoRenderError(const QString& message);
    void changePlaybackSpeed(double speed, bool syncOverride, bool clearSyncOverride = false);
    void updateHdrOutput(bool applySubtitleOptions);
    // Ask the window what it is presenting into and tell mpv, once a session
    // is under way and the scene graph therefore has a swapchain to ask.
    void updateRenderTarget();

public:
    // Both take effect on the next probe, which is the next time something
    // plays: the swapchain itself is fixed when the window is created.
    void setHdrOutputPreference(const QString& name);
    void setHdrPeakNits(int nits);

private:
    // The video's display size, tracked so a platform that shapes its own
    // video plane can be told. Both halves arrive as separate property
    // changes, so neither is acted on until the pair is complete.
    int m_videoWidth = 0;
    int m_videoHeight = 0;
    NativeAppWindow *m_window = nullptr;
    PlaybackSource *m_api = nullptr;
    PlaybackSession m_session;
    PlaybackReporter m_reporter;
    MpvLifecycle m_mpvLifecycle;
    quint64 m_mpvTeardownGeneration = 0;
    mpv_handle *m_idleMpvHandle = nullptr;
    bool m_idleMpvPreparationScheduled = false;
    bool m_idleMpvPreparationEnabled = true;
    QTimer m_progressTimer;
    QTimer m_backGuardTimer;
    QTimer m_uiPositionTimer;
    QTimer m_seekWatchdogTimer;
    QTimer m_backgroundTeardownTimer;
    bool m_visible = false;
    bool m_sessionActive = false;
    bool m_fileLoaded = false;
    bool m_seekDispatchReady = false;
    bool m_paused = false;
    bool m_buffering = false;
    int m_bufferingPercent = 0;
    bool m_seeking = false;
    bool m_pendingSeek = false;
    double m_pendingSeekTargetSeconds = 0.0;
    QByteArray m_pendingSeekFlags;
    bool m_debugOsdVisible = false;
    qint64 m_decoderDroppedFrames = 0;
    qint64 m_outputDroppedFrames = 0;
    // Frames the renderer presented late, and how fast it is actually
    // managing to put them up against how fast the file says they should
    // arrive. Dropped frames alone miss the failure that matters most: with
    // audio-synced video a renderer that cannot keep up does not drop
    // anything, it just runs slow and drags the picture behind the sound.
    qint64 m_delayedFrames = 0;
    double m_outputFps = 0.0;
    double m_containerFps = 0.0;
    MpvOptionProfile::RenderQuality m_renderQuality = MpvOptionProfile::RenderQuality::Balanced;
    bool m_directVideoOutput = false;
    bool m_hardwareDecoding = true;
    QByteArray m_softwareRenderer;
    // The opening seconds are where a device that cannot keep up says so:
    // the picture is being scaled and tone-mapped from the first frame, and
    // nothing has warmed a cache yet.
    QTimer m_renderStrainTimer;
    bool m_renderStrainReported = false;
    bool m_embeddedVideoOutput = false;
    PlaybackTrackState m_tracks;
    bool m_restoreStreamSelection = false;
    bool m_backAllowed = true;
    QString m_title;
    QString m_statusText = QStringLiteral("Ready");
    QString m_errorText;
    QString m_mediaKind = QStringLiteral("none");
    std::atomic_bool m_nightModeEnabled = false;
    std::atomic_bool m_toneMappingVisualizationEnabled = false;
    std::atomic<int> m_audioDelayMs = 0;
    std::atomic<int> m_fileAudioDelayMs = 0;
    std::atomic<int> m_subtitleDelayMs = 0;
    std::atomic<int> m_volume = 100;
    std::atomic_bool m_muted = false;
    double m_playbackSpeed = 1.0;
    double m_syncPlaybackSpeed = 1.0;
    bool m_syncPlaybackSpeedActive = false;
    QString m_audioOutputMode = QStringLiteral("auto");
    QByteArray m_automaticDemuxerMaxBytes = QByteArrayLiteral("64M");
    QByteArray m_demuxerMaxBytes = QByteArrayLiteral("64M");
    QByteArray m_demuxerMaxBackBytes = QByteArrayLiteral("32M");
    MpvConfigPolicy m_mpvConfigPolicy;
    bool m_activeUserMpvConfig = false;
    TlsTrustController *m_tlsTrust = nullptr;
    const QByteArray m_subtitleFontsPath;
    int m_forwardCacheSizeMiB = 0;
    SubtitlePreferences m_subtitlePreferences;
    bool m_hdrPlayback = false;
    bool m_hdrInput = false;
    RenderTargetProfile m_renderTarget;
    HdrOutputPreference m_hdrPreference = HdrOutputPreference::Auto;
    RenderTargetOverrides m_renderTargetOverrides;
    bool m_starfishVideoOutput = false;
    QByteArray m_targetTransfer;
    PlaybackPositionTracker m_positionTracker;
    PlaybackTimeline m_timeline;
    QStringList m_trickplaySheetUrls;
};

} // namespace Spool
