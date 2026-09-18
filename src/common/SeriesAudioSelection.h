#pragma once

#include "../media/MediaTypes.h"

namespace JellyfinNative {

struct SeriesAudioPreference {
    QString language;
    int languageTrackNumber = 0;

    bool isValid() const
    {
        return !language.isEmpty() && languageTrackNumber > 0;
    }

    friend bool operator==(const SeriesAudioPreference&, const SeriesAudioPreference&) = default;
};

SeriesAudioPreference seriesAudioPreferenceForSelection(const QList<MediaStreamInfo>& streams, int selectedStreamIndex);
int matchingSeriesAudioStreamIndex(const QList<MediaStreamInfo>& streams, const SeriesAudioPreference& preference);

} // namespace JellyfinNative
