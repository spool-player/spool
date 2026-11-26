#pragma once

#include "../media/MediaTypes.h"
#include "../platform/MpvConfigPolicy.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <vector>

struct mpv_handle;

namespace Spool {

struct MpvOption {
    QByteArray name;
    QByteArray value;
};

class MpvOptionProfile final {
public:
    enum class Platform {
        Desktop,
        Android,
        WebOS,
    };

    // How much work libplacebo is asked to do per frame. Until this existed
    // the GPU path ran at libplacebo's own defaults everywhere, which a
    // desktop absorbs and a television box does not.
    enum class RenderQuality {
        Maximum,
        High,
        Balanced,
        Fast,
    };

    struct NetworkProfile {
        int ringBytes;
        int rangeBytes;
        // libcurl receive buffer per transfer; each active request holds one.
        int readBytes;
    };

    struct StreamingOptions {
        bool enabled = true;
        int maxRequests = 4;
        int fixedRequests = 0;
        bool explore = true;
        bool allowExtraMemory = true;
        qint64 maxExtraBytes = 0;
        int receiveMode = 1;
        qint64 receiveBytes = 0;
        int httpMode = 0;
    };

    static NetworkProfile networkProfile(Platform platform);
    static bool applyRequestHeaders(mpv_handle *handle, QByteArray headers);
    static QByteArray renderQualityName(RenderQuality quality);
    static RenderQuality renderQualityFromName(const QString& name);
    static std::vector<MpvOption> renderQualityOptions(RenderQuality quality);
    static bool isHdrPlayback(const QList<MediaStreamInfo>& streams);
    static bool isHdrTransfer(const QByteArray& transfer);
    static bool isHdrOutput(bool starfishOutput, bool hdrInput, const QByteArray& targetTransfer);
    static QByteArray preloadedSubtitleStreams(const PlaybackSession& session, const QString& preferredLanguage);
    static QByteArray loadFileOptions(const PlaybackSession& session);
    static bool needsVideoSurface(const PlaybackSession& session);
    static bool useWebOSSoftwareVideo(const PlaybackSession& session);

    static QByteArray certificateBundle(const QStringList& candidates);
    static QByteArray systemCertificateBundle();
    static QByteArray inputKey(int key, int modifiers, const QString& text);

    static std::vector<MpvOption> preInitializeOptions(const MpvConfigPolicy& policy);
    static std::vector<MpvOption> applicationOptions(Platform platform, bool needsVideoSurface,
        const QString& audioOutputMode, const QByteArray& logPath, const StreamingOptions& streaming,
        const QByteArray& demuxerMaxBytes = QByteArrayLiteral("64M"),
        const QByteArray& demuxerMaxBackBytes = QByteArrayLiteral("32M"), bool embeddedVideo = false,
        const QByteArray& shaderCachePath = {}, const QByteArray& certificateBundlePath = {},
        RenderQuality quality = RenderQuality::Balanced, int alsaBufferMs = 40);
    static std::vector<MpvOption> subtitleOptions(
        const SubtitlePreferences& preferences, bool subtitlesEnabled, bool hdrPlayback = false);
};

} // namespace Spool
