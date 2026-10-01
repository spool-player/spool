#pragma once

#include <QList>
#include <QStringList>
#include <QVariantList>

struct mpv_node;

namespace Spool {

struct ParsedPlaybackTracks {
    QStringList subtitleLabels { QStringLiteral("Off") };
    QList<int> subtitleIds { -1 };
    int selectedSubtitleIndex = 0;
    QStringList audioLabels;
    QList<int> audioIds;
    int selectedAudioIndex = -1;
};

class PlaybackTrackParser final {
public:
    static ParsedPlaybackTracks parseTracks(const mpv_node *node);
    static QVariantList parseChapters(const mpv_node *node);
};

} // namespace Spool
