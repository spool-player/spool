#include "SeriesAudioSelection.h"
#include <algorithm>

namespace Spool {

namespace {

    bool isAudioStream(const MediaStreamInfo& stream)
    {
        return stream.type.compare(QStringLiteral("Audio"), Qt::CaseInsensitive) == 0;
    }

    QString primaryLanguageCode(QString language)
    {
        language = language.trimmed().toLower();
        const qsizetype dash = language.indexOf(QLatin1Char('-'));
        const qsizetype underscore = language.indexOf(QLatin1Char('_'));
        const qsizetype separator = dash < 0 ? underscore : (underscore < 0 ? dash : std::min(dash, underscore));
        if (separator >= 0)
            language.truncate(separator);
        return language;
    }

    bool languagesMatch(const QString& requested, const QString& available)
    {
        const QString a = primaryLanguageCode(requested);
        const QString b = primaryLanguageCode(available);
        return !a.isEmpty() && a == b;
    }

    const MediaStreamInfo *defaultAudioStream(const QList<MediaStreamInfo>& streams)
    {
        const MediaStreamInfo *firstAudio = nullptr;
        for (const MediaStreamInfo& stream : streams) {
            if (!isAudioStream(stream))
                continue;
            if (!firstAudio)
                firstAudio = &stream;
            if (stream.isDefault)
                return &stream;
        }
        return firstAudio;
    }

} // namespace

SeriesAudioPreference seriesAudioPreferenceForSelection(const QList<MediaStreamInfo>& streams, int selectedStreamIndex)
{
    const MediaStreamInfo *selected = nullptr;
    for (const MediaStreamInfo& stream : streams) {
        if (isAudioStream(stream) && stream.index == selectedStreamIndex) {
            selected = &stream;
            break;
        }
    }
    if (!selected || selected->isDefault || selected->language.trimmed().isEmpty())
        return {};

    int languageTrackNumber = 0;
    for (const MediaStreamInfo& stream : streams) {
        if (!isAudioStream(stream) || !languagesMatch(selected->language, stream.language))
            continue;
        ++languageTrackNumber;
        if (stream.index == selectedStreamIndex)
            return { selected->language.trimmed(), languageTrackNumber };
    }
    return {};
}

int matchingSeriesAudioStreamIndex(const QList<MediaStreamInfo>& streams, const SeriesAudioPreference& preference)
{
    if (!preference.isValid())
        return -1;

    const MediaStreamInfo *defaultStream = defaultAudioStream(streams);
    if (!defaultStream || languagesMatch(preference.language, defaultStream->language))
        return -1;

    int languageTrackNumber = 0;
    for (const MediaStreamInfo& stream : streams) {
        if (!isAudioStream(stream) || !languagesMatch(preference.language, stream.language))
            continue;
        ++languageTrackNumber;
        if (languageTrackNumber == preference.languageTrackNumber)
            return stream.index;
    }
    return -1;
}

} // namespace Spool
