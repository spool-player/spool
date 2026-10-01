#include "player/PlaybackTrackParser.h"

#include "TestMain.h"

extern "C" {
#include <mpv/client.h>
}

#include <QDebug>

#include <cstdlib>

using Spool::ParsedPlaybackTracks;
using Spool::PlaybackTrackParser;

namespace {

mpv_node stringNode(const char *value)
{
    mpv_node node {};
    node.format = MPV_FORMAT_STRING;
    node.u.string = const_cast<char *>(value);
    return node;
}

mpv_node intNode(int64_t value)
{
    mpv_node node {};
    node.format = MPV_FORMAT_INT64;
    node.u.int64 = value;
    return node;
}

mpv_node doubleNode(double value)
{
    mpv_node node {};
    node.format = MPV_FORMAT_DOUBLE;
    node.u.double_ = value;
    return node;
}

mpv_node flagNode(bool value)
{
    mpv_node node {};
    node.format = MPV_FORMAT_FLAG;
    node.u.flag = value ? 1 : 0;
    return node;
}

mpv_node listNode(mpv_format format, mpv_node_list *list)
{
    mpv_node node {};
    node.format = format;
    node.u.list = list;
    return node;
}

void require(bool condition, const char *message)
{
    if (condition)
        return;
    qCritical() << message;
    std::exit(EXIT_FAILURE);
}

} // namespace

SPOOL_TEST_MAIN("playback-track-parser")
{
    char *subtitleKeys[] = {
        const_cast<char *>("type"),
        const_cast<char *>("id"),
        const_cast<char *>("lang"),
        const_cast<char *>("title"),
        const_cast<char *>("codec"),
        const_cast<char *>("forced"),
        const_cast<char *>("external"),
        const_cast<char *>("selected"),
    };
    mpv_node subtitleValues[] = {
        stringNode("sub"),
        intNode(3),
        stringNode("eng"),
        stringNode("English [PGS]"),
        stringNode("hdmv_pgs_subtitle"),
        flagNode(true),
        flagNode(false),
        flagNode(true),
    };
    mpv_node_list subtitleMap {
        8,
        subtitleValues,
        subtitleKeys,
    };

    char *audioKeys[] = {
        const_cast<char *>("type"),
        const_cast<char *>("id"),
        const_cast<char *>("lang"),
        const_cast<char *>("title"),
        const_cast<char *>("codec"),
        const_cast<char *>("audio-channels"),
        const_cast<char *>("selected"),
    };
    mpv_node audioValues[] = {
        stringNode("audio"),
        intNode(7),
        stringNode("jpn"),
        stringNode("Main"),
        stringNode("aac"),
        intNode(6),
        flagNode(true),
    };
    mpv_node_list audioMap {
        7,
        audioValues,
        audioKeys,
    };

    mpv_node trackValues[] = {
        listNode(MPV_FORMAT_NODE_MAP, &subtitleMap),
        listNode(MPV_FORMAT_NODE_MAP, &audioMap),
    };
    mpv_node_list trackList {
        2,
        trackValues,
        nullptr,
    };
    const mpv_node tracksNode = listNode(MPV_FORMAT_NODE_ARRAY, &trackList);

    const ParsedPlaybackTracks tracks = PlaybackTrackParser::parseTracks(&tracksNode);
    require(tracks.subtitleIds == QList<int>({ -1, 3 }), "subtitle ids were not parsed");
    require(tracks.subtitleLabels == QStringList({ QStringLiteral("Off"), QStringLiteral("English (Forced) - PGS") }),
        "subtitle labels were not normalized");
    require(tracks.selectedSubtitleIndex == 1, "selected subtitle was not preserved");
    require(tracks.audioIds == QList<int>({ 7 }), "audio ids were not parsed");
    require(tracks.audioLabels == QStringList({ QStringLiteral("Japanese (Main) - 6ch, AAC") }),
        "audio labels were not normalized");
    require(tracks.selectedAudioIndex == 0, "selected audio track was not preserved");

    char *chapterKeys[] = {
        const_cast<char *>("title"),
        const_cast<char *>("time"),
    };
    mpv_node chapterValues[] = {
        stringNode(""),
        doubleNode(42.5),
    };
    mpv_node_list chapterMap {
        2,
        chapterValues,
        chapterKeys,
    };
    mpv_node chapters[] = {
        listNode(MPV_FORMAT_NODE_MAP, &chapterMap),
    };
    mpv_node_list chapterList {
        1,
        chapters,
        nullptr,
    };
    const mpv_node chaptersNode = listNode(MPV_FORMAT_NODE_ARRAY, &chapterList);

    const QVariantList parsedChapters = PlaybackTrackParser::parseChapters(&chaptersNode);
    require(parsedChapters.size() == 1, "chapter count was not parsed");
    const QVariantMap chapter = parsedChapters.front().toMap();
    require(chapter.value(QStringLiteral("title")).toString() == QStringLiteral("Chapter 1"),
        "empty chapter title did not receive a fallback");
    require(chapter.value(QStringLiteral("start")).toDouble() == 42.5, "chapter start was not parsed");

    return EXIT_SUCCESS;
}
