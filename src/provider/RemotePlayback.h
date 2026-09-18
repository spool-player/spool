#pragma once

#include "../app/SpoolRemoteProtocol.h"
#include "../media/MediaTypes.h"

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <vector>

namespace JellyfinNative {

// Playing on another client of the same source, and being played on by one.
// Outgoing: while a target is selected, every play request goes there
// instead of the local queue. Incoming: commands other clients send arrive
// as the signals below, in the source's own command envelope, already
// filtered by whether this device accepts them.
class RemotePlayback : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    virtual bool targetSelected() const = 0;
    // Sends the items to the selected target. False when no target is
    // selected, so the caller plays them locally.
    virtual bool playItems(const std::vector<MovieItem>& items, int startIndex, const QString& command, bool fromStart)
        = 0;
    // A Spool-to-Spool message that arrived through the source. True when it
    // was the peer link's own signalling and has been consumed.
    virtual bool handlePeerSignalling(const SpoolRemoteProtocol::Message& message) = 0;

signals:
    void playCommandReceived(const QJsonObject& data);
    void playstateCommandReceived(const QJsonObject& data);
    void generalCommandReceived(const QJsonObject& data);
    // A Spool-to-Spool message that arrived over the direct peer link.
    void peerMessageReceived(const JellyfinNative::SpoolRemoteProtocol::Message& message);
};

} // namespace JellyfinNative
