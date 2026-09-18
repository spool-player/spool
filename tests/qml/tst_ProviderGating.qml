import QtQuick
import QtTest
import "provider-capabilities" as Provider
import "../../qml/shell/TopBarNavigation.js" as TopBarNavigation

// The contract between shared QML and the provider registry: the capability
// names the shell gates on, and the rail's index space staying continuous when
// a gated-out button is missing, so Left and Right never land on nothing.
TestCase {
    name: "ProviderGating"

    readonly property var capabilityNames: ["auth", "discovery", "search", "userItemState", "playbackReporting",
        "segments", "libraryManagement", "syncPlay", "remoteControl", "quickConnect", "peerRelay", "streamQuality"]

    function test_capabilityNamesAreBooleans() {
        for (let index = 0; index < capabilityNames.length; ++index) {
            const name = capabilityNames[index]
            compare(typeof Provider.ProviderCapabilities[name], "boolean", name)
        }
    }

    function test_everyButtonTakesAnIndexWhenAllArePresent() {
        compare(TopBarNavigation.lastIndex(4, true, true, true), 6)
        compare(TopBarNavigation.slotAt(4, 4, true, true, true).kind, "remote")
        compare(TopBarNavigation.slotAt(5, 4, true, true, true).kind, "cast")
        compare(TopBarNavigation.slotAt(6, 4, true, true, true).kind, "sync")
        compare(TopBarNavigation.indexOf("sync", 4, true, true, true), 6)
    }

    function test_gatedOutButtonsTakeNoIndex() {
        // No SyncPlay: the rail ends at Cast and Right from Cast stays there.
        compare(TopBarNavigation.lastIndex(4, false, true, false), 4)
        compare(TopBarNavigation.slotAt(4, 4, false, true, false).kind, "cast")
        compare(TopBarNavigation.slotAt(5, 4, false, true, false).kind, "cast")
        compare(TopBarNavigation.indexOf("sync", 4, false, true, false), -1)

        // No remote control at all: the rail is only the navigation buttons.
        compare(TopBarNavigation.lastIndex(3, false, false, true), 3)
        compare(TopBarNavigation.slotAt(3, 3, false, false, true).kind, "sync")
        compare(TopBarNavigation.slotAt(9, 3, false, false, false).kind, "rail")
        compare(TopBarNavigation.slotAt(9, 3, false, false, false).railIndex, 2)
        compare(TopBarNavigation.indexOf("remote", 3, false, false, false), -1)
        compare(TopBarNavigation.indexOf("cast", 3, false, false, false), -1)
    }

    function test_indexesClampIntoTheRail() {
        compare(TopBarNavigation.slotAt(-3, 4, true, true, true).kind, "rail")
        compare(TopBarNavigation.slotAt(-3, 4, true, true, true).railIndex, 0)
        compare(TopBarNavigation.slotAt(0, 0, false, false, false), null)
        compare(TopBarNavigation.lastIndex(0, false, false, false), 0)
    }
}
