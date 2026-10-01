#include "PlaybackTrackState.h"

namespace Spool {

QStringList PlaybackTrackState::subtitleTracks() const
{
    return m_subtitleTracks;
}

bool PlaybackTrackState::subtitlesEnabled() const
{
    return m_subtitlesEnabled;
}

int PlaybackTrackState::selectedSubtitleIndex() const
{
    return m_selectedSubtitleIndex;
}

QStringList PlaybackTrackState::audioTracks() const
{
    return m_audioTracks;
}

int PlaybackTrackState::selectedAudioIndex() const
{
    return m_selectedAudioIndex;
}

QVariantList PlaybackTrackState::chapters() const
{
    return m_chapters;
}

bool PlaybackTrackState::hasChapters() const
{
    return m_chapters.size() > 1;
}

int PlaybackTrackState::currentChapter() const
{
    return m_currentChapter;
}

void PlaybackTrackState::resetForPlayback()
{
    m_subtitleTracks = { QStringLiteral("Off") };
    m_subtitleIds = { -1 };
    m_selectedSubtitleIndex = 0;
    m_subtitlesEnabled = true;
    m_audioTracks.clear();
    m_audioIds.clear();
    m_selectedAudioIndex = -1;
}

bool PlaybackTrackState::clearChapters()
{
    const bool changed = !m_chapters.isEmpty();
    m_chapters.clear();
    m_currentChapter = -1;
    return changed;
}

void PlaybackTrackState::applyParsedTracks(const ParsedPlaybackTracks& tracks)
{
    m_subtitleTracks = tracks.subtitleLabels;
    m_subtitleIds = tracks.subtitleIds;
    m_selectedSubtitleIndex = tracks.selectedSubtitleIndex;
    m_subtitlesEnabled = tracks.selectedSubtitleIndex > 0;
    m_audioTracks = tracks.audioLabels;
    m_audioIds = tracks.audioIds;
    m_selectedAudioIndex = tracks.selectedAudioIndex;
}

void PlaybackTrackState::setChapters(const QVariantList& chapters)
{
    m_chapters = chapters;
}

bool PlaybackTrackState::setCurrentChapter(int chapter)
{
    if (m_currentChapter == chapter)
        return false;
    m_currentChapter = chapter;
    return true;
}

std::optional<int> PlaybackTrackState::toggleSubtitleTarget() const
{
    if (m_selectedSubtitleIndex > 0)
        return 0;
    return m_subtitleTracks.size() > 1 ? std::optional<int>(1) : std::optional<int>(0);
}

std::optional<int> PlaybackTrackState::cycleSubtitleTarget() const
{
    if (m_subtitleTracks.size() <= 1)
        return std::nullopt;
    return (m_selectedSubtitleIndex + 1) % m_subtitleTracks.size();
}

std::optional<int> PlaybackTrackState::enableSubtitleTarget() const
{
    if (m_selectedSubtitleIndex > 0 || m_subtitleTracks.size() <= 1)
        return std::nullopt;
    return 1;
}

std::optional<int> PlaybackTrackState::cycleAudioTarget() const
{
    if (m_audioTracks.size() <= 1)
        return std::nullopt;
    return (m_selectedAudioIndex + 1) % m_audioTracks.size();
}

std::optional<QByteArrayList> PlaybackTrackState::subtitleCommand(int index) const
{
    if (index < 0 || index >= m_subtitleIds.size())
        return std::nullopt;

    const int trackId = m_subtitleIds[index];
    return QByteArrayList { QByteArrayLiteral("no-osd"), QByteArrayLiteral("set"), QByteArrayLiteral("sid"),
        trackId < 0 ? QByteArrayLiteral("no") : QByteArray::number(trackId) };
}

std::optional<QByteArrayList> PlaybackTrackState::audioCommand(int index) const
{
    if (index < 0 || index >= m_audioIds.size())
        return std::nullopt;

    const int trackId = m_audioIds[index];
    return QByteArrayList { QByteArrayLiteral("no-osd"), QByteArrayLiteral("set"), QByteArrayLiteral("aid"),
        trackId < 0 ? QByteArrayLiteral("no") : QByteArray::number(trackId) };
}

void PlaybackTrackState::applySubtitleSelection(int index)
{
    if (index < 0 || index >= m_subtitleIds.size())
        return;

    m_selectedSubtitleIndex = index;
    m_subtitlesEnabled = m_subtitleIds[index] >= 0;
}

void PlaybackTrackState::applyAudioSelection(int index)
{
    if (index < 0 || index >= m_audioIds.size())
        return;

    m_selectedAudioIndex = index;
}

} // namespace Spool
