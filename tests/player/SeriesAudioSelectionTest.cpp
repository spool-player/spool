#include "common/SeriesAudioSelection.h"

#include "TestMain.h"

#include <cstdlib>
#include <iostream>

using namespace Spool;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

MediaStreamInfo stream(int index, QString language, bool isDefault = false)
{
    MediaStreamInfo result;
    result.index = index;
    result.type = QStringLiteral("Audio");
    result.language = std::move(language);
    result.isDefault = isDefault;
    return result;
}

} // namespace

SPOOL_TEST_MAIN("series-audio-selection")
{
    MediaStreamInfo video;
    video.index = 0;
    video.type = QStringLiteral("Video");

    const QList<MediaStreamInfo> selectedEpisode {
        video,
        stream(1, QStringLiteral("ja"), true),
        stream(2, QStringLiteral("en")),
        stream(3, QStringLiteral("en-US")),
    };
    const SeriesAudioPreference preference = seriesAudioPreferenceForSelection(selectedEpisode, 3);
    require(preference.language == QStringLiteral("en-US") && preference.languageTrackNumber == 2,
        "a selected non-default track should retain its language-relative number");
    require(!seriesAudioPreferenceForSelection(selectedEpisode, 1).isValid(),
        "selecting the default track should clear the remembered override");

    const QList<MediaStreamInfo> nextEpisode {
        video,
        stream(4, QStringLiteral("fr"), true),
        stream(5, QStringLiteral("en-GB")),
        stream(7, QStringLiteral("en")),
    };
    require(matchingSeriesAudioStreamIndex(nextEpisode, preference) == 7,
        "a different-language default should be replaced by the same numbered language track");

    const QList<MediaStreamInfo> preferredLanguageDefault {
        video,
        stream(4, QStringLiteral("en"), true),
        stream(7, QStringLiteral("en-US")),
    };
    require(matchingSeriesAudioStreamIndex(preferredLanguageDefault, preference) == -1,
        "an already matching default language should not be overridden");

    const QList<MediaStreamInfo> missingNumber {
        video,
        stream(4, QStringLiteral("fr"), true),
        stream(5, QStringLiteral("en")),
    };
    require(matchingSeriesAudioStreamIndex(missingNumber, preference) == -1,
        "a missing language-relative track number should keep the episode default");

    return EXIT_SUCCESS;
}
