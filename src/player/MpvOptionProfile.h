#pragma once

#include "../media/MediaTypes.h"
#include "../platform/MpvConfigPolicy.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <vector>

namespace JellyfinNative {

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
        int parallelRequests;
    };

    static NetworkProfile networkProfile(Platform platform, int parallelRequests = 1);
    static QByteArray renderQualityName(RenderQuality quality);
    static RenderQuality renderQualityFromName(const QString& name);
    static std::vector<MpvOption> renderQualityOptions(RenderQuality quality);
    static bool isHdrPlayback(const QList<MediaStreamInfo>& streams);
    static bool isHdrTransfer(const QByteArray& transfer);
    static bool isHdrOutput(bool starfishOutput, bool hdrInput, const QByteArray& targetTransfer);
    static QByteArray preloadedSubtitleStreams(const PlaybackSession& session, const QString& preferredLanguage);
    static QByteArray loadFileOptions(const PlaybackSession& session);
    static bool useWebOSSoftwareVideo(const PlaybackSession& session);

    static QByteArray certificateBundle(const QStringList& candidates);
    static QByteArray systemCertificateBundle();
    static QByteArray inputKey(int key, int modifiers, const QString& text);

    static std::vector<MpvOption> preInitializeOptions(const MpvConfigPolicy& policy);
    static std::vector<MpvOption> applicationOptions(Platform platform, const QString& audioOutputMode,
        const QByteArray& logPath, const QByteArray& demuxerMaxBytes = QByteArrayLiteral("64M"),
        const QByteArray& demuxerMaxBackBytes = QByteArrayLiteral("32M"), int parallelRequests = 1,
        bool embeddedVideo = false, const QByteArray& shaderCachePath = {},
        const QByteArray& certificateBundlePath = {}, RenderQuality quality = RenderQuality::Balanced);
    static std::vector<MpvOption> subtitleOptions(
        const SubtitlePreferences& preferences, bool subtitlesEnabled, bool hdrPlayback = false);
};

} // namespace JellyfinNative
