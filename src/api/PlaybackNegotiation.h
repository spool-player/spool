#pragma once
#include "../media/MediaTypes.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace JellyfinNative {

struct PlaybackSelection {
    QJsonObject source;
    QString playMethod;
};

class PlaybackNegotiation final {
public:
    static PlaybackSelection selectSource(const QJsonArray& mediaSources, bool preferRemux);
    static QString buildUrl(const QString& serverUrl, const QString& itemId, const PlaybackSelection& selection);
    static TrickplayInfo selectTrickplay(
        const QJsonObject& trickplay, const QString& mediaSourceId, int preferredWidth);
    // maxHeight caps the vertical resolution the server may send. It reaches
    // Jellyfin as a codec-profile condition rather than a URL parameter,
    // because the transcode URL is the server's to write: given the condition
    // it scales the stream down and puts MaxHeight in the URL itself. A
    // bitrate ceiling alone leaves the source resolution untouched, which is
    // how picking 480p used to lower the bitrate and nothing else. Zero means
    // no ceiling.
    static QJsonObject buildDeviceProfile(qint64 maxStreamingBitrate, int maxHeight = 0,
        const QStringList& videoCodecs = {}, bool restrictVideoCodecs = false);
};

} // namespace JellyfinNative
