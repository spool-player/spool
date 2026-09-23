#pragma once

#include "PlaybackTrackParser.h"

#include <QByteArray>
#include <QByteArrayList>
#include <QList>
#include <QStringList>
#include <QVariantList>

#include <optional>

namespace Spool {

class PlaybackTrackState final {
public:
    QStringList subtitleTracks() const;
    bool subtitlesEnabled() const;
    int selectedSubtitleIndex() const;
    QStringList audioTracks() const;
    int selectedAudioIndex() const;
    QVariantList chapters() const;
    bool hasChapters() const;
    int currentChapter() const;

    void resetForPlayback();
    bool clearChapters();
    void applyParsedTracks(const ParsedPlaybackTracks& tracks);
    void setChapters(const QVariantList& chapters);
    bool setCurrentChapter(int chapter);

    std::optional<int> toggleSubtitleTarget() const;
    std::optional<int> cycleSubtitleTarget() const;
    std::optional<int> enableSubtitleTarget() const;
    std::optional<int> cycleAudioTarget() const;

    std::optional<QByteArrayList> subtitleCommand(int index) const;
    std::optional<QByteArrayList> audioCommand(int index) const;
    void applySubtitleSelection(int index);
    void applyAudioSelection(int index);

private:
    QStringList m_subtitleTracks { QStringLiteral("Off") };
    QList<int> m_subtitleIds { -1 };
    int m_selectedSubtitleIndex = 0;
    bool m_subtitlesEnabled = true;
    QStringList m_audioTracks;
    QList<int> m_audioIds;
    int m_selectedAudioIndex = -1;
    QVariantList m_chapters;
    int m_currentChapter = -1;
};

} // namespace Spool
