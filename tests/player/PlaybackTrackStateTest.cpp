#include "player/PlaybackTrackState.h"

#include "TestMain.h"

#include <QDebug>
#include <QVariantMap>

#include <cstdlib>

using Spool::ParsedPlaybackTracks;
using Spool::PlaybackTrackState;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    qCritical() << message;
    std::exit(EXIT_FAILURE);
}

bool commandEquals(const std::optional<QByteArrayList>& command, const QByteArrayList& expected)
{
    return command.has_value() && *command == expected;
}

} // namespace

SPOOL_TEST_MAIN("playback-track-state")
{
    PlaybackTrackState state;

    ParsedPlaybackTracks tracks;
    tracks.subtitleLabels = { QStringLiteral("Off"), QStringLiteral("English"), QStringLiteral("French") };
    tracks.subtitleIds = { -1, 3, 4 };
    tracks.selectedSubtitleIndex = 1;
    tracks.audioLabels = { QStringLiteral("Stereo"), QStringLiteral("Commentary") };
    tracks.audioIds = { 1, 2 };
    tracks.selectedAudioIndex = 0;
    state.applyParsedTracks(tracks);

    require(state.subtitlesEnabled(), "selected subtitle track should enable subtitles");
    require(state.toggleSubtitleTarget() == 0, "toggle should turn subtitles off");
    require(state.cycleSubtitleTarget() == 2, "cycle should advance subtitle index");
    require(state.cycleAudioTarget() == 1, "cycle should advance audio index");
    require(commandEquals(state.subtitleCommand(0),
                { QByteArrayLiteral("no-osd"), QByteArrayLiteral("set"), QByteArrayLiteral("sid"),
                    QByteArrayLiteral("no") }),
        "off subtitle command was wrong");
    require(commandEquals(state.subtitleCommand(2),
                { QByteArrayLiteral("no-osd"), QByteArrayLiteral("set"), QByteArrayLiteral("sid"),
                    QByteArrayLiteral("4") }),
        "subtitle command used wrong mpv id");
    require(commandEquals(state.audioCommand(1),
                { QByteArrayLiteral("no-osd"), QByteArrayLiteral("set"), QByteArrayLiteral("aid"),
                    QByteArrayLiteral("2") }),
        "audio command used wrong mpv id");

    state.applySubtitleSelection(0);
    require(!state.subtitlesEnabled(), "off subtitle selection should disable subtitles");
    require(state.enableSubtitleTarget() == 1, "enable subtitles should select first real track");

    QVariantList chapters;
    chapters.push_back(
        QVariantMap { { QStringLiteral("title"), QStringLiteral("Chapter 1") }, { QStringLiteral("start"), 0.0 } });
    chapters.push_back(
        QVariantMap { { QStringLiteral("title"), QStringLiteral("Chapter 2") }, { QStringLiteral("start"), 30.0 } });
    state.setChapters(chapters);
    require(state.hasChapters(), "two chapters should enable chapter actions");
    require(state.setCurrentChapter(1), "chapter change should report changed");
    require(!state.setCurrentChapter(1), "same chapter should not report changed");
    require(state.clearChapters(), "clearing populated chapters should report changed");
    require(!state.hasChapters(), "cleared chapters should disable chapter actions");

    state.resetForPlayback();
    require(state.subtitleTracks() == QStringList({ QStringLiteral("Off") }),
        "playback reset should restore subtitle labels");
    require(state.subtitlesEnabled(), "playback reset should allow the next file to auto-select subtitles");
    require(state.audioTracks().isEmpty(), "playback reset should clear audio labels");

    return EXIT_SUCCESS;
}
