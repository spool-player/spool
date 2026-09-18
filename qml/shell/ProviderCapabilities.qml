pragma Singleton
import QtQuick

// Which optional provider capabilities the active media source serves. Shared
// shell and page QML gates every provider-specific control on one of these,
// so a source that has no SyncPlay or remote control simply has no such
// buttons, and nothing else changes.
//
// Placeholder: every flag is true until the Phase 3 C++ provider registry
// replaces this file with a qmlRegisterSingletonInstance under the same name
// and the same property names. The contract is the names.
QtObject {
    // The provider supplies its own login flow and session (Session).
    readonly property bool auth: true
    // Servers can be found on the local network (Discovery, DiscoveredServers).
    readonly property bool discovery: true
    // Text search and suggestions (Search).
    readonly property bool search: true
    // Favourite, played and playback position live on the source (ItemState).
    readonly property bool userItemState: true
    // Playback start, progress and stop are reported back to the source.
    readonly property bool playbackReporting: true
    // Intro and credit markers for the skip cards.
    readonly property bool segments: true
    // Playlists, collections, rename and delete (Management).
    readonly property bool libraryManagement: true
    // Group watch with a shared clock (SyncPlay).
    readonly property bool syncPlay: true
    // Casting to and driving other sessions of the same source (RemoteControl).
    readonly property bool remoteControl: true
    // Sign-in by code on another device (QuickConnect).
    readonly property bool quickConnect: true
    // A datagram path between two Spool instances relayed by the source.
    readonly property bool peerRelay: true
}
