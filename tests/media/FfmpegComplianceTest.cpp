#include "TestMain.h"
#define __STDC_CONSTANT_MACROS
extern "C" {
#include <libavcodec/bsf.h>
#include <libavcodec/codec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
}

#include "FfmpegCapabilities.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

template <size_t N> bool contains(const std::array<std::string_view, N>& values, std::string_view value)
{
    return std::find(values.cbegin(), values.cend(), value) != values.cend();
}

std::string_view primaryName(const char *names)
{
    const std::string_view value = names ? names : "";
    return value.substr(0, value.find(','));
}

void require(bool condition, const std::string& message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

template <size_t N>
void requireNoUnexpected(
    const std::array<std::string_view, N>& allowed, std::string_view category, const auto& eachComponent)
{
    eachComponent([&](std::string_view name) {
        require(contains(allowed, name),
            "FFmpeg runtime exposes unlisted " + std::string(category) + ": " + std::string(name));
    });
}

void requireFixtureOpens(std::string_view relativePath, std::string_view expectedFormat)
{
    const std::string path = std::string(TEST_MEDIA_DIR) + "/" + std::string(relativePath);
    AVFormatContext *context = nullptr;
    require(avformat_open_input(&context, path.c_str(), nullptr, nullptr) >= 0,
        "FFmpeg could not open playback fixture: " + path);
    require(context->iformat && primaryName(context->iformat->name) == expectedFormat,
        "FFmpeg selected the wrong demuxer for playback fixture: " + path);
    require(
        avformat_find_stream_info(context, nullptr) >= 0, "FFmpeg could not inspect playback fixture streams: " + path);
    avformat_close_input(&context);
}

} // namespace

SPOOL_TEST_MAIN("ffmpeg-compliance")
{
    const std::string license = avutil_license();
    const std::string configuration = avutil_configuration();

    if (FfmpegCapabilities::kGplEnabled) {
        require(license.starts_with("GPL"), "FFmpeg must report a GPL license");
        require(configuration.find("--enable-gpl") != std::string::npos,
            "GPL FFmpeg platforms must explicitly enable GPL code");
        require(configuration.find("--disable-gpl") < configuration.find("--enable-gpl"),
            "the explicit platform GPL policy must follow the disabled license baseline");
    } else {
        require(license.starts_with("LGPL"), "FFmpeg must report an LGPL license");
        require(
            configuration.find("--enable-gpl") == std::string::npos, "LGPL FFmpeg platforms must not enable GPL code");
    }
    require(configuration.find("--disable-everything") != std::string::npos,
        "FFmpeg must start from a disable-everything feature set");
    require(configuration.find("--disable-autodetect") != std::string::npos,
        "FFmpeg must disable dependency autodetection");
    require(configuration.find("--disable-gpl") != std::string::npos, "FFmpeg must explicitly disable GPL code");
    require(configuration.find("--disable-version3") != std::string::npos,
        "FFmpeg must explicitly disable version-3-only code");
    require(
        configuration.find("--disable-nonfree") != std::string::npos, "FFmpeg must explicitly disable non-free code");
    require(configuration.find("--enable-version3") == std::string::npos,
        "FFmpeg configuration must not contain an inherited version-3 enable flag");
    require(configuration.find("--enable-nonfree") == std::string::npos,
        "FFmpeg configuration must not contain an inherited non-free enable flag");

    for (const std::string_view name : FfmpegCapabilities::kDemuxers)
        require(
            av_find_input_format(name.data()) != nullptr, "FFmpeg is missing allowed demuxer: " + std::string(name));
    for (const std::string_view name : FfmpegCapabilities::kDecoders)
        require(avcodec_find_decoder_by_name(name.data()) != nullptr,
            "FFmpeg is missing allowed decoder: " + std::string(name));
    for (const std::string_view name : FfmpegCapabilities::kHardwareAccelerators) {
        require(configuration.find("--enable-hwaccel=" + std::string(name)) != std::string::npos,
            "FFmpeg is missing configured hardware accelerator: " + std::string(name));
    }
    for (const std::string_view name : FfmpegCapabilities::kFilters)
        require(avfilter_get_by_name(name.data()) != nullptr, "FFmpeg is missing allowed filter: " + std::string(name));
    for (const std::string_view name : FfmpegCapabilities::kMuxers)
        require(av_guess_format(name.data(), nullptr, nullptr) != nullptr,
            "FFmpeg is missing allowed muxer: " + std::string(name));
    for (const std::string_view name : FfmpegCapabilities::kBitstreamFilters)
        require(av_bsf_get_by_name(name.data()) != nullptr,
            "FFmpeg is missing allowed bitstream filter: " + std::string(name));
    for (const std::string_view name : FfmpegCapabilities::kForbiddenImageDecoders)
        require(avcodec_find_decoder_by_name(name.data()) == nullptr,
            "FFmpeg must not expose image decoder: " + std::string(name));

    requireNoUnexpected(FfmpegCapabilities::kDemuxers, "demuxer", [](const auto& visit) {
        void *opaque = nullptr;
        while (const AVInputFormat *format = av_demuxer_iterate(&opaque))
            visit(primaryName(format->name));
    });
    requireNoUnexpected(FfmpegCapabilities::kDecoders, "decoder", [](const auto& visit) {
        void *opaque = nullptr;
        while (const AVCodec *codec = av_codec_iterate(&opaque)) {
            if (av_codec_is_decoder(codec))
                visit(codec->name);
        }
    });
    requireNoUnexpected(FfmpegCapabilities::kFilters, "filter", [](const auto& visit) {
        void *opaque = nullptr;
        while (const AVFilter *filter = av_filter_iterate(&opaque))
            visit(filter->name);
    });
    requireNoUnexpected(FfmpegCapabilities::kMuxers, "muxer", [](const auto& visit) {
        void *opaque = nullptr;
        while (const AVOutputFormat *format = av_muxer_iterate(&opaque))
            visit(primaryName(format->name));
    });
    requireNoUnexpected(FfmpegCapabilities::kBitstreamFilters, "bitstream filter", [](const auto& visit) {
        void *opaque = nullptr;
        while (const AVBitStreamFilter *filter = av_bsf_iterate(&opaque))
            visit(filter->name);
    });
    requireNoUnexpected(FfmpegCapabilities::kProtocols, "input protocol", [](const auto& visit) {
        void *opaque = nullptr;
        while (const char *protocol = avio_enum_protocols(&opaque, 0))
            visit(protocol);
    });
    for (const std::string_view name : FfmpegCapabilities::kProtocols) {
        bool present = false;
        void *opaque = nullptr;
        while (const char *protocol = avio_enum_protocols(&opaque, 0))
            present = present || protocol == name;
        require(present, "FFmpeg is missing allowed protocol: " + std::string(name));
    }
    // libcurl performs every network transfer, not FFmpeg, so it is tempting to
    // build FFmpeg without its network protocols. lavf's HLS demuxer resolves
    // each playlist and segment URL by name first and refuses the stream when
    // the lookup comes back empty, whatever io_open would have done with it.
    // Repeat that lookup here: an FFmpeg without https plays direct streams
    // perfectly and cannot play a single transcode.
    require(primaryName(avio_find_protocol_name("https://example.invalid/master.m3u8")) == "https",
        "FFmpeg cannot resolve https, so lavf's HLS demuxer will reject every transcoded stream");
    requireFixtureOpens("direct-mpeg2.mkv", "matroska");
    requireFixtureOpens("remux-h264.mp4", "mov");
    requireFixtureOpens("transcode.m3u8", "hls");
    requireFixtureOpens("audio.flac", "flac");
    requireFixtureOpens("subtitle.vtt", "webvtt");

    return EXIT_SUCCESS;
}
