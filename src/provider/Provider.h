#pragma once

#include <QFlags>
#include <QObject>
#include <QString>
#include <QVariantMap>

namespace Spool {

class ArtworkSource;
class Catalog;
class PlaybackSource;
class SearchSource;
class StreamQualityControl;
class UserItemStateSink;

// A source of media: one account on a server, a debrid service, a folder of
// files. The app itself sees exactly one Provider, the SourceHub, which
// routes to every enabled account; each account is a Provider too.
//
// Optional interfaces are null without the matching capability, and shared
// QML gates each optional control on the ProviderCapabilities singleton.
class Provider : public QObject {
    Q_OBJECT

public:
    enum Capability : unsigned {
        Search = 1u << 0,
        // Favourite, played and resume position live on the source.
        UserItemState = 1u << 1,
        // Playback start, progress and stop are reported back.
        PlaybackReporting = 1u << 2,
        // Intro and credit markers for the skip cards.
        Segments = 1u << 3,
        // Watching together on a shared clock.
        GroupPlayback = 1u << 4,
        // Other clients can send this one play and transport commands.
        RemoteControl = 1u << 5,
        // A lower bitrate or resolution than stored can be requested.
        StreamQuality = 1u << 6,
        // Scrubbing previews.
        Trickplay = 1u << 7,
    };
    Q_DECLARE_FLAGS(Capabilities, Capability)
    Q_FLAG(Capabilities)

    using QObject::QObject;

    virtual QString id() const = 0;
    virtual QString displayName() const = 0;
    virtual Capabilities capabilities() const = 0;

    virtual PlaybackSource *playback() = 0;
    virtual Catalog *catalog() = 0;
    virtual ArtworkSource *artwork() = 0;
    virtual SearchSource *search()
    {
        return nullptr;
    }
    virtual UserItemStateSink *itemState()
    {
        return nullptr;
    }
    virtual StreamQualityControl *streamQuality()
    {
        return nullptr;
    }

    // Whether the catalog can be used now; sessionStarted() follows once it
    // can.
    virtual bool ready() const = 0;
    virtual void shutdown() { }

signals:
    void capabilitiesChanged();
    void sessionStarted();
    void sessionEnded();
    void errorOccurred(const QString& message);
    void toastRequested(const QString& message);
    // Something changed the library; an empty id means "whatever is shown".
    void contentChanged(const QString& changedItemId);
    // Everything else the source pushes: group state, remote commands.
    void sourceEvent(const QString& type, const QVariantMap& payload);
};

Q_DECLARE_OPERATORS_FOR_FLAGS(Provider::Capabilities)

} // namespace Spool
