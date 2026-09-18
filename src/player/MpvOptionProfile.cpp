#include "MpvOptionProfile.h"

#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QStringList>
#include <QUrl>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace JellyfinNative {

QByteArray MpvOptionProfile::inputKey(int key, int modifiers, const QString& text)
{
    QByteArray name;
    const bool surrogatePair = text.size() == 2 && text.at(0).isHighSurrogate() && text.at(1).isLowSurrogate();
    if (text.size() > 1 && !surrogatePair)
        return {}; // IME commits are text, not a single mpv key.
    // Qt swaps Control and Meta on macOS; mpv names the physical modifiers.
#ifdef Q_OS_MACOS
    const bool control = modifiers & Qt::ControlModifier;
    const bool meta = modifiers & Qt::MetaModifier;
    modifiers &= ~(Qt::ControlModifier | Qt::MetaModifier);
    if (control)
        modifiers |= Qt::MetaModifier;
    if (meta)
        modifiers |= Qt::ControlModifier;
#endif
    switch (key) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_AltGr:
        return {};
    case Qt::Key_Space:
        name = "SPACE";
        break;
    case Qt::Key_Return:
        name = "ENTER";
        break;
    case Qt::Key_Enter:
        name = "KP_ENTER";
        break;
    case Qt::Key_Escape:
        name = "ESC";
        break;
    case Qt::Key_Tab:
        name = "TAB";
        break;
    case Qt::Key_Backtab:
        name = "TAB";
        modifiers |= Qt::ShiftModifier;
        break;
    case Qt::Key_Backspace:
        name = "BS";
        break;
    case Qt::Key_Delete:
        name = "DEL";
        break;
    case Qt::Key_Insert:
        name = "INS";
        break;
    case Qt::Key_Home:
        name = "HOME";
        break;
    case Qt::Key_End:
        name = "END";
        break;
    case Qt::Key_PageUp:
        name = "PGUP";
        break;
    case Qt::Key_PageDown:
        name = "PGDWN";
        break;
    case Qt::Key_Left:
        name = "LEFT";
        break;
    case Qt::Key_Right:
        name = "RIGHT";
        break;
    case Qt::Key_Up:
        name = "UP";
        break;
    case Qt::Key_Down:
        name = "DOWN";
        break;
    case Qt::Key_MediaPlay:
        name = "PLAY";
        break;
    case Qt::Key_MediaPause:
        name = "PAUSE";
        break;
    case Qt::Key_MediaTogglePlayPause:
        name = "PLAYPAUSE";
        break;
    case Qt::Key_MediaStop:
        name = "STOP";
        break;
    case Qt::Key_MediaNext:
        name = "NEXT";
        break;
    case Qt::Key_MediaPrevious:
        name = "PREV";
        break;
    default:
        if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
            name = "F" + QByteArray::number(key - Qt::Key_F1 + 1);
        } else if ((text.size() == 1 && text.at(0).isPrint())
            || (surrogatePair && QChar::isPrint(QChar::surrogateToUcs4(text.at(0), text.at(1))))) {
            name = text.toUtf8();
            modifiers &= ~Qt::ShiftModifier; // Shift is already represented by the character.
        } else if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            name = QByteArray(1, char((modifiers & Qt::ShiftModifier) ? key : key + ('a' - 'A')));
            modifiers &= ~Qt::ShiftModifier;
        } else if (key >= Qt::Key_Exclam && key <= Qt::Key_AsciiTilde) {
            name = QByteArray(1, char(key));
        } else {
            return {};
        }
    }
    if (modifiers & Qt::KeypadModifier) {
        if (key >= Qt::Key_0 && key <= Qt::Key_9) {
            name = "KP" + QByteArray::number(key - Qt::Key_0);
        } else {
            switch (key) {
            case Qt::Key_Plus:
                name = "KP_ADD";
                break;
            case Qt::Key_Minus:
                name = "KP_SUBTRACT";
                break;
            case Qt::Key_Asterisk:
                name = "KP_MULTIPLY";
                break;
            case Qt::Key_Slash:
                name = "KP_DIVIDE";
                break;
            case Qt::Key_Period:
            case Qt::Key_Comma:
                name = "KP_DEC";
                break;
            case Qt::Key_Delete:
            case Qt::Key_Insert:
            case Qt::Key_Home:
            case Qt::Key_End:
            case Qt::Key_PageUp:
            case Qt::Key_PageDown:
            case Qt::Key_Left:
            case Qt::Key_Right:
            case Qt::Key_Up:
            case Qt::Key_Down:
                name.prepend("KP_");
                break;
            default:
                break;
            }
        }
    }
    QByteArray prefix;
    if (modifiers & Qt::ControlModifier)
        prefix += "Ctrl+";
    if (modifiers & Qt::AltModifier)
        prefix += "Alt+";
    if (modifiers & Qt::MetaModifier)
        prefix += "Meta+";
    if (modifiers & Qt::ShiftModifier)
        prefix += "Shift+";
    return prefix + name;
}

namespace {

    QLocale::Language languageFromCode(QString code)
    {
        code = code.trimmed();
        QLocale::Language language = QLocale::codeToLanguage(QStringView(code));
        if (language != QLocale::AnyLanguage)
            return language;

        const qsizetype hyphen = code.indexOf(QLatin1Char('-'));
        const qsizetype underscore = code.indexOf(QLatin1Char('_'));
        const qsizetype separator = hyphen < 0 ? underscore : (underscore < 0 ? hyphen : qMin(hyphen, underscore));
        if (separator > 0)
            language = QLocale::codeToLanguage(QStringView(code).left(separator));
        return language;
    }

    bool languagesMatch(const QString& requested, const QString& available)
    {
        const QString requestedCode = requested.trimmed();
        const QString availableCode = available.trimmed();
        if (requestedCode.isEmpty() || availableCode.isEmpty())
            return false;
        if (requestedCode.compare(availableCode, Qt::CaseInsensitive) == 0)
            return true;

        const QLocale::Language requestedLanguage = languageFromCode(requestedCode);
        return requestedLanguage != QLocale::AnyLanguage && requestedLanguage == languageFromCode(availableCode);
    }

    QByteArray mpvBool(bool value)
    {
        return value ? QByteArrayLiteral("yes") : QByteArrayLiteral("no");
    }

    QByteArray mpvArgbColor(const QString& rgb, QByteArray fallback)
    {
        QString color = rgb.trimmed();
        if (color.startsWith(QLatin1Char('#')))
            color.remove(0, 1);
        if (color.size() != 6)
            return fallback;

        for (const QChar ch : color) {
            if (!ch.isDigit() && (ch.toLower() < QLatin1Char('a') || ch.toLower() > QLatin1Char('f')))
                return fallback;
        }

        return QByteArrayLiteral("#FF") + color.toUpper().toLatin1();
    }

    QByteArray scaledSubtitleColor(const QString& rgb, int percent)
    {
        QString color = rgb.trimmed();
        if (color.startsWith(QLatin1Char('#')))
            color.remove(0, 1);
        bool ok = false;
        const uint value = color.toUInt(&ok, 16);
        if (!ok || color.size() != 6)
            return QByteArrayLiteral("#FFFFFFFF");

        const int scale = qBound(5, percent, 100);
        const auto channel = [scale](uint component) { return qBound(0, qRound(component * scale / 100.0), 255); };
        return (QStringLiteral("#FF%1%2%3")
                    .arg(channel((value >> 16) & 0xff), 2, 16, QLatin1Char('0'))
                    .arg(channel((value >> 8) & 0xff), 2, 16, QLatin1Char('0'))
                    .arg(channel(value & 0xff), 2, 16, QLatin1Char('0'))
                    .toUpper())
            .toLatin1();
    }

    QByteArray subtitleFontFamily(const QString& value)
    {
        if (value == QStringLiteral("interface"))
            return QByteArrayLiteral("IBM Plex Sans Var");
        if (value.startsWith(QStringLiteral("system:")))
            return value.sliced(7).toUtf8();
        return QByteArrayLiteral("Atkinson Hyperlegible");
    }

    QByteArray subtitleBackgroundColor(const QString& value)
    {
        if (value == QStringLiteral("opaque"))
            return QByteArrayLiteral("#FF000000");
        if (value == QStringLiteral("translucent"))
            return QByteArrayLiteral("#A0000000");
        return QByteArrayLiteral("#00000000");
    }

    struct SubtitleShadowOptions {
        QByteArray borderSize = QByteArrayLiteral("3.5");
        QByteArray shadowOffset = QByteArrayLiteral("1");
        QByteArray shadowColor = QByteArrayLiteral("#80000000");
    };

    SubtitleShadowOptions subtitleShadowOptions(const QString& value)
    {
        SubtitleShadowOptions options;
        if (value == QStringLiteral("none")) {
            options.shadowOffset = QByteArrayLiteral("0");
            options.shadowColor = QByteArrayLiteral("#00000000");
        } else if (value == QStringLiteral("raised")) {
            options.shadowOffset = QByteArrayLiteral("1");
            options.shadowColor = QByteArrayLiteral("#A0000000");
        } else if (value == QStringLiteral("depressed")) {
            options.shadowOffset = QByteArrayLiteral("-1");
            options.shadowColor = QByteArrayLiteral("#A0000000");
        } else if (value == QStringLiteral("uniform")) {
            options.borderSize = QByteArrayLiteral("4.5");
            options.shadowOffset = QByteArrayLiteral("0");
            options.shadowColor = QByteArrayLiteral("#00000000");
        }
        return options;
    }

} // namespace

bool MpvOptionProfile::isHdrPlayback(const QList<MediaStreamInfo>& streams)
{
    for (const MediaStreamInfo& stream : streams) {
        if (stream.type.compare(QStringLiteral("Video"), Qt::CaseInsensitive) != 0)
            continue;
        const QString metadata
            = (stream.videoRange + QLatin1Char(' ') + stream.colorTransfer + QLatin1Char(' ') + stream.profile)
                  .toLower();
        if (metadata.contains(QStringLiteral("hdr")) || metadata.contains(QStringLiteral("dovi"))
            || metadata.contains(QStringLiteral("dolby")) || metadata.contains(QStringLiteral("2084"))
            || metadata.contains(QStringLiteral("pq")) || metadata.contains(QStringLiteral("b67"))
            || metadata.contains(QStringLiteral("hlg")))
            return true;
    }
    return false;
}
bool MpvOptionProfile::isHdrTransfer(const QByteArray& transfer)
{
    const QByteArray normalized = transfer.trimmed().toLower();
    return normalized == QByteArrayLiteral("pq") || normalized == QByteArrayLiteral("hlg")
        || normalized.contains("2084") || normalized.contains("b67");
}

bool MpvOptionProfile::isHdrOutput(bool starfishOutput, bool hdrInput, const QByteArray& targetTransfer)
{
    return starfishOutput ? hdrInput : isHdrTransfer(targetTransfer);
}

QByteArray MpvOptionProfile::preloadedSubtitleStreams(const PlaybackSession& session, const QString& preferredLanguage)
{
    if (session.playMethod.compare(QStringLiteral("DirectPlay"), Qt::CaseInsensitive) != 0
        || preferredLanguage.trimmed().isEmpty())
        return {};

    QList<int> indexes;
    for (const MediaStreamInfo& stream : session.mediaStreams) {
        if (stream.index < 0 || stream.isExternal
            || stream.type.compare(QStringLiteral("Subtitle"), Qt::CaseInsensitive) != 0
            || !languagesMatch(preferredLanguage, stream.language))
            continue;
        if (!indexes.contains(stream.index))
            indexes.push_back(stream.index);
    }

    QByteArrayList values;
    values.reserve(indexes.size());
    for (int index : indexes)
        values.push_back(QByteArray::number(index));
    return values.join(',');
}

MpvOptionProfile::NetworkProfile MpvOptionProfile::networkProfile(Platform platform, int parallelRequests)
{
    const int requests = std::clamp(parallelRequests, 1, 4);
    return platform == Platform::WebOS ? NetworkProfile { 2 * 1024 * 1024, 512 * 1024, requests }
                                       : NetworkProfile { 4 * 1024 * 1024, 1024 * 1024, requests };
}

QByteArray MpvOptionProfile::loadFileOptions(const PlaybackSession& session)
{
    if (session.playMethod.compare(QStringLiteral("Transcode"), Qt::CaseInsensitive) != 0)
        return {};

    const QUrl url(session.url);
    if (!url.path().endsWith(QStringLiteral(".m3u8"), Qt::CaseInsensitive))
        return {};

    // Jellyfin's HLS master manifest is a non-seekable HTTP response. Tell
    // lavf what it is up front so mpv does not repeatedly probe and seek the
    // small manifest back to byte zero before HLS can open its media playlist.
    return QByteArrayLiteral("demuxer=lavf,demuxer-lavf-format=hls,initial-audio-sync=no");
}

std::vector<MpvOption> MpvOptionProfile::preInitializeOptions(const MpvConfigPolicy& policy)
{
    std::vector<MpvOption> options {
        { "config", policy.mode == MpvConfigPolicy::Mode::Disabled ? "no" : "yes" },
        { "script-opts", "stats-redraw_delay=2" },
    };
    if (policy.mode == MpvConfigPolicy::Mode::Custom)
        options.push_back({ "config-dir", policy.directory.toUtf8() });
    return options;
}

bool MpvOptionProfile::useWebOSSoftwareVideo(const PlaybackSession& session)
{
    if (session.playMethod.compare(QStringLiteral("Transcode"), Qt::CaseInsensitive) == 0)
        return false;

    static const QStringList softwareCodecs { QStringLiteral("h263"), QStringLiteral("mpeg1video"),
        QStringLiteral("mpeg2video"), QStringLiteral("mpeg4"), QStringLiteral("vc1") };
    for (const MediaStreamInfo& stream : session.mediaStreams) {
        if (stream.type.compare(QStringLiteral("Video"), Qt::CaseInsensitive) == 0)
            return softwareCodecs.contains(stream.codec.trimmed(), Qt::CaseInsensitive);
    }
    return false;
}

// libcurl carries every byte of playback, so its trust store is the one that
// has to be right. Only Windows finds it on its own, through Schannel and the
// system certificate store; the OpenSSL builds elsewhere look at a compiled-in
// path that is correct for the machine that built them and not for the device
// running them. Name the bundle explicitly instead, honouring the environment
// override first so a sandbox or a Nix wrapper can point playback at the same
// certificates as the rest of the process.
QByteArray MpvOptionProfile::certificateBundle(const QStringList& candidates)
{
    for (const QString& candidate : candidates) {
        if (!candidate.isEmpty() && QFileInfo(candidate).isFile())
            return QFile::encodeName(candidate);
    }
    return {};
}

QByteArray MpvOptionProfile::systemCertificateBundle()
{
#if defined(Q_OS_WIN)
    // Schannel reads the Windows certificate store; a bundle file would only
    // freeze today's roots into the installer.
    return {};
#else
    QStringList candidates {
        qEnvironmentVariable("SSL_CERT_FILE"),
        qEnvironmentVariable("CURL_CA_BUNDLE"),
        QStringLiteral("/etc/ssl/certs/ca-certificates.crt"),
        QStringLiteral("/etc/pki/tls/certs/ca-bundle.crt"),
        QStringLiteral("/etc/ssl/ca-bundle.pem"),
        QStringLiteral("/etc/ssl/cert.pem"),
        QStringLiteral("/usr/local/etc/openssl/cert.pem"),
    };
    return certificateBundle(candidates);
#endif
}

std::vector<MpvOption> MpvOptionProfile::applicationOptions(Platform platform, const QString& audioOutputMode,
    const QByteArray& logPath, const QByteArray& demuxerMaxBytes, const QByteArray& demuxerMaxBackBytes,
    int parallelRequests, bool embeddedVideo, const QByteArray& shaderCachePath,
    const QByteArray& certificateBundlePath, RenderQuality quality)
{
    const bool webOS = platform == Platform::WebOS;
    const bool android = platform == Platform::Android;
    // embeddedVideo means "we render it ourselves", which is the Qt scene
    // graph everywhere and, on webOS, the software-decode path it reaches for
    // legacy codecs. It used to be clamped to webOS here because nowhere else
    // had a second option; Android does now.
    const NetworkProfile network = networkProfile(platform, parallelRequests);
    const QString normalizedAudioOutput = webOS ? audioOutputMode : normalizedAudioOutputMode(audioOutputMode);
    const bool starfishAudio = webOS && !embeddedVideo
        && (audioOutputMode == QStringLiteral("starfish") || audioOutputMode == QStringLiteral("starfish-pcm"));

    std::vector<MpvOption> options {
        { "terminal", "no" },
        { "msg-level", webOS ? "all=warn,starfish=info,sub=v" : "all=warn,sub=v" },
        { "log-file", logPath },
        { "demuxer-lavf-analyzeduration", "1" },
        { "demuxer-lavf-probesize", "1048576" },
        { "cache", "yes" },
        { "cache-pause", "no" },
        { "demuxer-max-bytes", demuxerMaxBytes },
        { "demuxer-max-back-bytes", demuxerMaxBackBytes },
        { "curl-enabled", "yes" },
        { "curl-buffer-size", QByteArray::number(network.ringBytes) },
        { "curl-max-request-size", QByteArray::number(network.rangeBytes) },
        { "curl-parallel-requests", QByteArray::number(network.parallelRequests) },
        { "force-window", "no" },
    };
    if (!certificateBundlePath.isEmpty())
        options.push_back({ "tls-ca-file", certificateBundlePath });
    if (!webOS) {
        options.push_back({ "ytdl", "no" });
        if (!shaderCachePath.isEmpty())
            options.push_back({ "gpu-shader-cache-dir", shaderCachePath });
    }

    if (webOS) {
        options.push_back({ "initial-audio-sync", embeddedVideo ? "yes" : "no" });
        options.push_back({ "vo", embeddedVideo ? "libmpv" : "starfish" });
        options.push_back({ "vd", embeddedVideo ? "lavc" : "starfish" });
        options.push_back({ "ao", starfishAudio ? "starfish,null" : "alsa,null" });
        if (!embeddedVideo)
            options.push_back({ "vo-starfish-audio-hint", starfishAudio ? "yes" : "no" });
        else {
            options.push_back({ "hwdec", "no" });
            options.push_back({ "vd-lavc-threads", "3" });
            options.push_back({ "scale", "bilinear" });
            options.push_back({ "cscale", "bilinear" });
            options.push_back({ "dscale", "bilinear" });
            options.push_back({ "correct-downscaling", "no" });
            options.push_back({ "linear-downscaling", "no" });
            options.push_back({ "sigmoid-upscaling", "no" });
            options.push_back({ "deband", "no" });
            options.push_back({ "interpolation", "no" });
            options.push_back({ "dither-depth", "no" });
            options.push_back({ "deinterlace", "auto" });
        }
        if (!starfishAudio) {
            options.push_back({ "audio-device", "alsa/hw:0,7" });
            options.push_back({ "video-sync", "display-resample" });
        }
        options.push_back({ "audio-channels", "stereo" });
        options.push_back({ "audio-format", starfishAudio ? "s16" : "s32" });
        options.push_back({ "audio-samplerate", starfishAudio ? "192000" : "48000" });
        if (!starfishAudio) {
            options.push_back({ "audio-buffer", embeddedVideo ? "0.100" : "0.050" });
            options.push_back({ "alsa-buffer-time", "40000" });
            options.push_back({ "alsa-periods", "8" });
            options.push_back({ "alsa-no-hw-pause", "yes" });
            options.push_back({ "alsa-bounded-io", "yes" });
        } else {
            options.push_back({ "ao-starfish-feed-ahead", "0.4" });
        }
    } else if (android && !embeddedVideo) {
        // Direct output. MediaCodec decodes into a Surface of its own and the
        // system compositor puts the interface on top, so no frame is ever
        // read back, uploaded, scaled or tone-mapped by us. That is the whole
        // point on a television box: a Mali-G31 cannot shade 4K sixty times a
        // second, and asking it to leaves even the player's own controls
        // stuttering. The display pipeline does the scaling and the HDR.
        //
        // No render-quality options here: libplacebo is not in this path, so
        // there is nothing for them to tune.
        options.push_back({ "vo", "mediacodec_embed" });
        options.push_back({ "hwdec", "mediacodec" });
        options.push_back({ "audio-fallback-to-null", "yes" });
        if (normalizedAudioOutput != QStringLiteral("auto"))
            options.push_back({ "ao", normalizedAudioOutput.toUtf8() });
        else
            options.push_back({ "ao", "audiotrack,opensles,null" });
    } else {
        options.push_back({ "vo", "libmpv" });
        options.push_back({ "audio-fallback-to-null", "yes" });
        // Everything above this line ran at libplacebo's defaults before,
        // which is a great deal of work for a Mali-class part to do sixty
        // times a second.
        const std::vector<MpvOption> renderOptions = renderQualityOptions(quality);
        options.insert(options.end(), renderOptions.begin(), renderOptions.end());
        if (android) {
            // Zero-copy MediaCodec hands libplacebo an external-OES frame that
            // the renderer walks off the end of: playback dies inside
            // pl_render_image on the scene graph's render thread within
            // seconds. That path needs mpv to own an Android Surface, which it
            // cannot while it renders through the Qt scene graph, so decode in
            // hardware and read the frames back.
            options.push_back({ "hwdec", "mediacodec-copy" });
        } else {
#if defined(Q_OS_LINUX)
            options.push_back({ "hwdec", "auto-copy" });
#else
            options.push_back({ "hwdec", "auto-safe" });
#endif
        }
        if (normalizedAudioOutput != QStringLiteral("auto")) {
            options.push_back({ "ao", normalizedAudioOutput.toUtf8() });
        } else if (android) {
            // AAudio is mpv's first choice on Android, but its clock stops
            // driving the core after a seek, which freezes the picture while
            // the file still reports itself as playing. AudioTrack is the
            // mature Android backend and restarts cleanly.
            options.push_back({ "ao", "audiotrack,opensles,null" });
        }
    }

    const MpvOption applicationOptions[] = {
        { "osd-bar", "no" },
        { "osd-duration", "0" },
        { "load-stats-overlay", "yes" },
        { "audio-file-auto", "no" },
        { "input-default-bindings", "no" },
        { "input-vo-keyboard", "no" },
        { "keep-open", "no" },
        { "idle", "yes" },
    };
    options.insert(options.end(), std::begin(applicationOptions), std::end(applicationOptions));
    if (!webOS) {
        const MpvOption desktopScriptOptions[] = {
            { "osc", "no" },
            { "load-console", "no" },
            { "load-select", "no" },
            { "load-positioning", "no" },
            { "load-commands", "no" },
            { "load-context-menu", "no" },
        };
        options.insert(options.end(), std::begin(desktopScriptOptions), std::end(desktopScriptOptions));
    }
    return options;
}

QByteArray MpvOptionProfile::renderQualityName(RenderQuality quality)
{
    switch (quality) {
    case RenderQuality::Maximum:
        return QByteArrayLiteral("maximum");
    case RenderQuality::High:
        return QByteArrayLiteral("high");
    case RenderQuality::Balanced:
        return QByteArrayLiteral("balanced");
    case RenderQuality::Fast:
        return QByteArrayLiteral("fast");
    }
    return QByteArrayLiteral("balanced");
}

MpvOptionProfile::RenderQuality MpvOptionProfile::renderQualityFromName(const QString& name)
{
    const QString normalized = name.trimmed().toLower();
    if (normalized == QStringLiteral("maximum"))
        return RenderQuality::Maximum;
    if (normalized == QStringLiteral("high"))
        return RenderQuality::High;
    if (normalized == QStringLiteral("fast"))
        return RenderQuality::Fast;
    return RenderQuality::Balanced;
}

// The four rungs differ in the three things that actually cost a frame:
// how many taps the scalers take, whether the picture is resampled in linear
// light, and whether the tone curve is fitted to measured peaks. Everything
// else follows from those.
std::vector<MpvOption> MpvOptionProfile::renderQualityOptions(RenderQuality quality)
{
    switch (quality) {
    case RenderQuality::Maximum:
        return {
            { "scale", "ewa_lanczossharp" },
            { "cscale", "ewa_lanczossharp" },
            { "dscale", "mitchell" },
            { "correct-downscaling", "yes" },
            { "linear-downscaling", "yes" },
            { "sigmoid-upscaling", "yes" },
            { "deband", "yes" },
            { "dither-depth", "auto" },
            { "tone-mapping", "bt.2446a" },
            { "hdr-compute-peak", "yes" },
        };
    case RenderQuality::High:
        return {
            { "scale", "lanczos" },
            { "cscale", "lanczos" },
            { "dscale", "mitchell" },
            { "correct-downscaling", "yes" },
            { "linear-downscaling", "yes" },
            { "sigmoid-upscaling", "yes" },
            { "deband", "no" },
            { "dither-depth", "auto" },
            { "tone-mapping", "auto" },
            { "hdr-compute-peak", "yes" },
        };
    case RenderQuality::Balanced:
        return {
            { "scale", "spline36" },
            { "cscale", "bilinear" },
            { "dscale", "bilinear" },
            { "correct-downscaling", "no" },
            { "linear-downscaling", "no" },
            { "sigmoid-upscaling", "no" },
            { "deband", "no" },
            { "interpolation", "no" },
            { "dither-depth", "auto" },
            { "tone-mapping", "auto" },
            { "hdr-compute-peak", "no" },
        };
    case RenderQuality::Fast:
        // Deliberately the same shape as the webOS software-decode profile:
        // nothing per-pixel beyond a bilinear tap.
        return {
            { "scale", "bilinear" },
            { "cscale", "bilinear" },
            { "dscale", "bilinear" },
            { "correct-downscaling", "no" },
            { "linear-downscaling", "no" },
            { "sigmoid-upscaling", "no" },
            { "deband", "no" },
            { "interpolation", "no" },
            { "dither-depth", "no" },
            { "tone-mapping", "hable" },
            { "hdr-compute-peak", "no" },
        };
    }
    return {};
}

std::vector<MpvOption> MpvOptionProfile::subtitleOptions(
    const SubtitlePreferences& preferences, bool subtitlesEnabled, bool hdrPlayback)
{
    const SubtitlePreferences prefs = preferences;
    const QString subtitleMode = prefs.mode.isEmpty() ? QStringLiteral("Default") : prefs.mode;
    const bool noSubtitles = subtitleMode == QStringLiteral("None");
    const bool onlyForced = subtitleMode == QStringLiteral("OnlyForced");
    const bool alwaysPlay = subtitleMode == QStringLiteral("Always");
    const bool smart = subtitleMode == QStringLiteral("Smart");
    const QString audioLanguage
        = prefs.audioLanguage.trimmed().isEmpty() ? prefs.language.trimmed() : prefs.audioLanguage.trimmed();
    const bool smartAudio = prefs.audioMode == QStringLiteral("Smart") && !audioLanguage.isEmpty();
    const bool overrideEmbeddedStyling = prefs.styling.compare(QStringLiteral("custom"), Qt::CaseInsensitive) == 0;
    const bool overrideGeometry = prefs.alwaysOverridePositionAndSize;
    const QByteArray assOverride = overrideEmbeddedStyling ? QByteArrayLiteral("force")
        : overrideGeometry                                 ? QByteArrayLiteral("scale")
                                                           : QByteArrayLiteral("no");
    const int vertical = qBound(0, prefs.verticalPosition, 100);
    const SubtitleShadowOptions shadow = subtitleShadowOptions(prefs.dropShadow);
    const QByteArray subtitleColor = hdrPlayback ? scaledSubtitleColor(prefs.textColor, prefs.hdrBrightnessPercent)
                                                 : mpvArgbColor(prefs.textColor, QByteArrayLiteral("#FFFFFFFF"));
    const int bitmapSharpness = qBound(0, prefs.bitmapSharpnessPercent, 100);
    const QByteArray bitmapSoftness = QByteArray::number(1.4 - bitmapSharpness * 0.0065, 'f', 2);
    const bool recolorImages = prefs.recolorImageSubtitles;
    const QByteArray disabledColor = QByteArrayLiteral("#00000000");

    return {
        { "sid", !subtitlesEnabled || noSubtitles ? QByteArrayLiteral("no") : QByteArrayLiteral("auto") },
        { "slang", prefs.language.toUtf8() },
        { "alang", smartAudio ? audioLanguage.toUtf8() : QByteArray() },
        { "sub-auto", "all" },
        { "sub-visibility", mpvBool(!noSubtitles) },
        { "sub-forced-events-only", mpvBool(onlyForced) },
        { "subs-with-matching-audio", mpvBool(alwaysPlay) },
        { "subs-fallback", mpvBool(!noSubtitles && !onlyForced && !smart) },
        { "subs-fallback-forced", "yes" },
        { "sub-ass", "yes" },
        { "sub-ass-override", assOverride },
        { "sub-ass-override-colors", mpvBool(prefs.overrideTextColor) },
        { "sub-scale-signs", mpvBool(overrideGeometry) },
        { "sub-use-margins", mpvBool(prefs.allowSubtitlesInBlackBars) },
        { "sub-ass-force-margins", mpvBool(prefs.allowSubtitlesInBlackBars) },
        { "sub-font", subtitleFontFamily(prefs.font) },
        { "sub-font-size", "55" },
        { "sub-scale-by-window", "yes" },
        { "sub-scale-with-window", "yes" },
        { "sub-ass-scale-with-window", "yes" },
        { "sub-scale", QByteArray::number(qBound(50, prefs.scalePercent, 200) / 100.0, 'f', 2) },
        { "sub-gauss", "0.0" },
        { "sub-sdf-softness", bitmapSoftness },
        { "sub-sdf-shadow", mpvBool(prefs.bitmapShadowEnabled) },
        { "sub-sdf-shadow-core-sigma", QByteArray::number(qBound(1, prefs.bitmapShadowCoreSize, 4)) },
        { "sub-sdf-shadow-core-grow", QByteArray::number(qBound(0, prefs.bitmapShadowCoreGrow, 4)) },
        { "sub-sdf-shadow-core-opacity",
            QByteArray::number(qBound(0, prefs.bitmapShadowCoreOpacityPercent, 100) / 100.0, 'f', 2) },
        { "sub-sdf-shadow-spread", mpvBool(prefs.bitmapShadowSpreadEnabled) },
        { "sub-sdf-shadow-spread-sigma", QByteArray::number(qBound(1, prefs.bitmapShadowSpreadSize, 16)) },
        { "sub-sdf-shadow-spread-grow", QByteArray::number(qBound(0, prefs.bitmapShadowSpreadGrow, 8)) },
        { "sub-sdf-shadow-spread-x", QByteArray::number(qBound(-16, prefs.bitmapShadowSpreadX, 16)) },
        { "sub-sdf-shadow-spread-y", QByteArray::number(qBound(-16, prefs.bitmapShadowSpreadY, 16)) },
        { "sub-sdf-shadow-spread-opacity",
            QByteArray::number(qBound(0, prefs.bitmapShadowSpreadOpacityPercent, 100) / 100.0, 'f', 2) },
        { "sub-sdf-shadow-dither", mpvBool(prefs.bitmapShadowDither) },
        { "sub-bold", mpvBool(prefs.textWeight == QStringLiteral("bold")) },
        { "sub-pos", QByteArray::number(vertical) },
        // Text and image subtitles have to sit in the same place at the same
        // setting. An image subtitle is moved by its ink box, so at 100 it
        // touches the bottom of the picture; libass falls back to the style's
        // bottom margin there, which left text a whole line higher than the
        // burned-in subtitles of the same film. No margin, no discrepancy: the
        // vertical position means the same thing for both.
        { "sub-margin-y", "0" },
        { "sub-color", subtitleColor },
        { "sub-border-size", shadow.borderSize },
        { "sub-border-color", "#FF000000" },
        { "sub-shadow-offset", shadow.shadowOffset },
        { "sub-shadow-color", shadow.shadowColor },
        { "sub-back-color", subtitleBackgroundColor(prefs.textBackground) },
        { "sub-image-color", recolorImages ? subtitleColor : disabledColor },
        { "sub-image-color-mode", "replace" },
        { "sub-image-outline-color", recolorImages ? QByteArrayLiteral("#FF000000") : disabledColor },
        { "sub-image-position", overrideGeometry ? QByteArrayLiteral("all") : QByteArrayLiteral("bottom-block") },
    };
}

} // namespace JellyfinNative
