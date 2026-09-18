#pragma once

#include <QFlags>
#include <QObject>
#include <QString>

namespace JellyfinNative {

class PlaybackSource;

// A source of media the app can sit on: a Jellyfin server, a debrid account,
// a folder of files. The shell and pages are the same whichever one is
// active; what differs is which optional capabilities the source serves, and
// every provider-specific control in shared QML is gated on one of these
// through the ProviderCapabilities singleton the registry publishes.
//
// The catalog, search, artwork and user-item-state accessors arrive with the
// contract branch; until then those controllers still take the Jellyfin
// facade directly.
class Provider : public QObject {
    Q_OBJECT

public:
    enum Capability : unsigned {
        // The provider supplies its own login flow and session (Session).
        Auth = 1u << 0,
        // Servers can be found on the local network (Discovery, DiscoveredServers).
        Discovery = 1u << 1,
        // Text search and suggestions (Search).
        Search = 1u << 2,
        // Favourite, played and playback position live on the source (ItemState).
        UserItemState = 1u << 3,
        // Playback start, progress and stop are reported back to the source.
        PlaybackReporting = 1u << 4,
        // Intro and credit markers for the skip cards.
        Segments = 1u << 5,
        // Playlists, collections, rename and delete (Management).
        LibraryManagement = 1u << 6,
        // Group watch with a shared clock (SyncPlay).
        SyncPlay = 1u << 7,
        // Casting to and driving other sessions of the same source (RemoteControl).
        RemoteControl = 1u << 8,
        // Sign-in by code on another device (QuickConnect).
        QuickConnect = 1u << 9,
        // A datagram path between two Spool instances relayed by the source.
        PeerRelay = 1u << 10,
    };
    Q_DECLARE_FLAGS(Capabilities, Capability)
    Q_FLAG(Capabilities)

    using QObject::QObject;

    virtual QString id() const = 0;
    virtual QString displayName() const = 0;
    virtual Capabilities capabilities() const = 0;
    virtual PlaybackSource *playback() = 0;
    // Registers the QML singletons only this provider's own QML reaches
    // (Session, SyncPlay and the like). Shared QML never names them without
    // first checking the matching capability.
    virtual void registerQmlSingletons() = 0;

signals:
    void capabilitiesChanged();
};

Q_DECLARE_OPERATORS_FOR_FLAGS(Provider::Capabilities)

} // namespace JellyfinNative
