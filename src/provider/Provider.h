#pragma once

#include "../app/AccountProfile.h"

#include <QFlags>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <exception>
#include <vector>

namespace JellyfinNative {

class ArtworkService;
class ArtworkSource;
class BrowseSessionController;
class Catalog;
class GroupPlayback;
class PlayQueueController;
class PlaybackSource;
class PlayerController;
class RemotePlayback;
class SearchSource;
class SettingsController;
class StreamQualityControl;
class UserItemStateSink;

// A source of media the app can sit on: a Jellyfin server, a debrid account,
// a folder of files. The shell and pages are the same whichever one is
// active; what differs is which optional capabilities the source serves, and
// every provider-specific control in shared QML is gated on one of these
// through the ProviderCapabilities singleton the registry publishes.
//
// The app talks to a provider only through the interfaces returned here and
// the signals below. A provider that needs nothing from the session-shaped
// hooks (no login, no server) leaves the defaults in place: it is ready from
// the start and never signs out.
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
        // The source can be asked for a lower bitrate or resolution than it
        // holds, so the player offers a quality ladder.
        StreamQuality = 1u << 11,
    };
    Q_DECLARE_FLAGS(Capabilities, Capability)
    Q_FLAG(Capabilities)

    // The core objects a provider's session-bound parts sit on. Anything the
    // provider creates from these is parented to `owner`, which must not
    // outlive `player`.
    struct CoreServices {
        QObject *owner = nullptr;
        PlayerController *player = nullptr;
        PlayQueueController *playQueue = nullptr;
        SettingsController *settings = nullptr;
        BrowseSessionController *browse = nullptr;
        ArtworkService *artwork = nullptr;
    };

    using QObject::QObject;

    virtual QString id() const = 0;
    virtual QString displayName() const = 0;
    virtual Capabilities capabilities() const = 0;

    // Every provider serves these three.
    virtual PlaybackSource *playback() = 0;
    virtual Catalog *catalog() = 0;
    virtual ArtworkSource *artwork() = 0;
    // Null unless the matching capability is set.
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
    virtual GroupPlayback *groupPlayback()
    {
        return nullptr;
    }
    virtual RemotePlayback *remotePlayback()
    {
        return nullptr;
    }

    // Called once the core objects exist, before anything is played.
    virtual void attach(const CoreServices&) { }
    // Registers the QML singletons only this provider's own QML reaches
    // (Session, SyncPlay and the like). Shared QML never names them without
    // first checking the matching capability.
    virtual void registerQmlSingletons() = 0;
    virtual void shutdown() { }

    // Whether the catalog can be used right now. A source that needs a
    // session reports false until it has one and emits sessionStarted() when
    // it does; a source that needs none is ready from construction.
    virtual bool ready() const = 0;
    // Whether a stored account will sign in without the viewer's help. The
    // shell holds its first route until this settles.
    virtual bool hasDefaultProfile() const
    {
        return true;
    }
    // The local settings keys the provider wants read before the first
    // frame, handed back through restoreFromStorage() with the saved
    // accounts. Returns what hasDefaultProfile() will report.
    virtual QStringList startupStorageKeys() const
    {
        return {};
    }
    virtual bool restoreFromStorage(QVariantMap, std::vector<AccountProfile>)
    {
        return true;
    }
    // The identity this instance presents to a server, unique per running
    // instance on the same machine.
    virtual void setDeviceId(const QString&) { }
    // The viewer's BCP 47 locale, for sources that localise their strings.
    virtual void setLocale(const QString&) { }
    // True when the error meant the session is gone and the provider has
    // already started signing out, so the caller need not report it.
    virtual bool handleUnauthorized(const std::exception_ptr&)
    {
        return false;
    }

signals:
    void capabilitiesChanged();
    // ready() flipped: the catalog became usable, or stopped being so.
    void sessionStarted();
    void sessionEnded();
    // The viewer is opening a stored account, switching accounts, or
    // signing out; the app clears what it was showing for the last one.
    void activationStarted();
    void switchUserRequested();
    void signOutStarted();
    void hasDefaultProfileChanged();
    void busyChanged(bool busy, const QString& text);
    void errorOccurred(const QString& message);
    void toastRequested(const QString& message);
    // Something the provider did changed the library; an empty id means
    // "whatever is on screen".
    void contentChanged(const QString& changedItemId);
};

Q_DECLARE_OPERATORS_FOR_FLAGS(Provider::Capabilities)

} // namespace JellyfinNative
