pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"
import "../providers/jellyfin"
import "TopBarNavigation.js" as TopBarNavigation

// Horizontal top navigation bar. Hosts primary routes on the left and playback
// destination, remote-control, and SyncPlay actions on the right. D-pad:
// Left/Right move between bar items, Down enters the content area, and pages
// return focus here by pressing Up at their top edge.
FocusScope {
    id: root
    property string currentRoute: "home"
    // Which edge of the shell the rail is fixed to. It moves to the bottom
    // where the viewport is narrow enough that the top of it is out of reach
    // of a thumb, and its vertical keys follow it.
    property string edge: "top"
    signal navigate(string route)
    signal contentRequested

    readonly property bool syncPlayMenuOpen: syncMenuLoader.item ? syncMenuLoader.item.menuOpen : false
    property bool syncPlayMenuLoaded: false
    readonly property bool remoteControlMenuOpen: remoteMenuLoader.item ? remoteMenuLoader.item.menuOpen : false
    property bool remoteControlMenuLoaded: false
    // Provider-shaped singletons are only touched behind their capability, so
    // a source without them never has the bindings evaluated.
    readonly property var session: ProviderCapabilities.auth ? Session : null
    readonly property var remoteControl: ProviderCapabilities.remoteControl ? RemoteControl : null
    readonly property bool remoteTargetSelected: remoteControl ? remoteControl.targetSelected : false
    readonly property string remoteTargetName: remoteControl ? remoteControl.selectedTargetName : ""
    readonly property bool castVisible: remoteControl !== null && Settings.castButtonEnabled
    readonly property bool remoteVisible: remoteTargetSelected
    readonly property var syncPlay: ProviderCapabilities.syncPlay ? SyncPlay : null
    readonly property bool syncVisible: syncPlay !== null
    readonly property bool syncActive: syncPlay ? syncPlay.enabled : false
    readonly property var syncGroups: syncPlay ? syncPlay.groups : []
    readonly property bool syncAvailable: syncGroups && syncGroups.length > 0
    readonly property string selectedRoute: currentRoute === "libraryGrid" ? "home" : currentRoute

    // A rail cell is never narrower than the button inside it. It used to be
    // a flat scaled(50) while the button took the touch-target floor, so on a
    // phone a 72px button sat in a 41px cell: the buttons overlapped by more
    // than half their width and a tap near a boundary went to whichever
    // happened to be stacked last. With a pointer the reverse held, and the
    // few pixels either side of each button were dead.
    readonly property int railCellWidth: Math.max(Metrics.scaled(50), Metrics.touchTargetPx)

    // Index space: navigation buttons, then the optional Remote, Cast and
    // SyncPlay buttons. TopBarNavigation.js owns the arithmetic so a gated-out
    // button drops out of the sequence rather than leaving a dead index.
    function lastIndex() {
        return TopBarNavigation.lastIndex(railRepeater.count, remoteVisible, castVisible, syncVisible)
    }

    function slotIndex(kind) {
        return TopBarNavigation.indexOf(kind, railRepeater.count, remoteVisible, castVisible, syncVisible)
    }

    function slotAt(index) {
        return TopBarNavigation.slotAt(index, railRepeater.count, remoteVisible, castVisible, syncVisible)
    }

    function focusedIndex() {
        for (let i = 0; i < railRepeater.count; ++i) {
            const item = railRepeater.itemAt(i)
            if (item && item.hasButtonFocus())
                return i
        }
        if (remoteVisible && remoteButton.activeFocus)
            return slotIndex("remote")
        if (castVisible && castButton.activeFocus)
            return slotIndex("cast")
        if (syncVisible && syncButton.activeFocus)
            return slotIndex("sync")
        return 0
    }

    function focusIndex(index) {
        const slot = slotAt(index)
        if (!slot)
            return
        if (slot.kind === "rail") {
            const item = railRepeater.itemAt(slot.railIndex)
            if (item)
                item.forceButtonFocus()
        } else if (slot.kind === "remote") {
            InputKeys.focus(remoteButton)
        } else if (slot.kind === "cast") {
            InputKeys.focus(castButton)
        } else {
            InputKeys.focus(syncButton)
        }
    }

    function focusCurrent() {
        for (let i = 0; i < railRepeater.count; ++i) {
            const item = railRepeater.itemAt(i)
            if (item && item.route === selectedRoute) {
                item.forceButtonFocus()
                return
            }
        }
        const first = railRepeater.itemAt(0)
        if (first)
            first.forceButtonFocus()
    }

    function remoteMenu() {
        remoteControlMenuLoaded = true
        return remoteMenuLoader.item
    }

    function openRemoteMenu() {
        if (!castVisible)
            return
        closeSyncPlayMenu(false)
        const menu = remoteMenu()
        if (menu)
            menu.openMenu()
    }

    function closeRemoteMenu(restoreFocus) {
        const menu = remoteMenuLoader.item
        if (menu)
            menu.closeMenu()
        if (restoreFocus !== false && castVisible)
            InputKeys.focus(castButton)
    }

    function syncMenu() {
        syncPlayMenuLoaded = true
        return syncMenuLoader.item
    }

    function openSyncMenu() {
        if (!syncVisible)
            return
        closeRemoteMenu(false)
        syncPlay.refreshGroups()
        const menu = syncMenu()
        if (menu)
            menu.openMenu()
    }

    function closeSyncPlayMenu(restoreFocus) {
        const menu = syncMenuLoader.item
        if (menu)
            menu.closeMenu()
        if (restoreFocus !== false && syncVisible)
            InputKeys.focus(syncButton)
    }

    function closeMenus(restoreFocus) {
        if (remoteControlMenuOpen)
            closeRemoteMenu(restoreFocus)
        if (syncPlayMenuOpen)
            closeSyncPlayMenu(restoreFocus)
    }

    function containsSyncPlayPoint(item, x, y) {
        const buttonPoint = syncButton.mapFromItem(item, x, y)
        if (syncButton.contains(buttonPoint))
            return true
        const menu = syncMenuLoader.item
        if (!menu || !menu.menuOpen)
            return false
        const menuPoint = menu.mapFromItem(item, x, y)
        return menu.contains(menuPoint)
    }

    function containsRemoteControlPoint(item, x, y) {
        const buttonPoint = castButton.mapFromItem(item, x, y)
        if (castButton.contains(buttonPoint))
            return true
        if (remoteVisible) {
            const remoteButtonPoint = remoteButton.mapFromItem(item, x, y)
            if (remoteButton.contains(remoteButtonPoint))
                return true
        }
        const menu = remoteMenuLoader.item
        if (!menu || !menu.menuOpen)
            return false
        const menuPoint = menu.mapFromItem(item, x, y)
        return menu.contains(menuPoint)
    }

    function activate() {
        const remote = remoteMenuLoader.item
        if (remote && remote.menuOpen) {
            remote.activate()
            return
        }
        const sync = syncMenuLoader.item
        if (sync && sync.menuOpen) {
            sync.activate()
            return
        }
        const slot = slotAt(focusedIndex())
        if (!slot)
            return
        if (slot.kind === "remote") {
            closeRemoteMenu(false)
            navigate("remoteControl")
        } else if (slot.kind === "cast") {
            openRemoteMenu()
        } else if (slot.kind === "sync") {
            openSyncMenu()
        } else {
            const item = railRepeater.itemAt(slot.railIndex)
            if (item)
                navigate(item.route)
        }
    }

    function back() {
        if (remoteControlMenuOpen) {
            closeRemoteMenu()
            return true
        }
        if (syncPlayMenuOpen) {
            closeSyncPlayMenu()
            return true
        }
        return false
    }

    onActiveFocusChanged: if (!activeFocus)
                              closeMenus(false)

    function routeKey(key, phase, repeat) {
        const remote = remoteMenuLoader.item
        if (remote && remote.menuOpen)
            return remote.routeKey(key, phase, repeat)
        const sync = syncMenuLoader.item
        if (sync && sync.menuOpen)
            return sync.routeKey(key, phase, repeat)
        // The rail leaves towards wherever the content is, which is below it
        // on a page and above it once it has moved down within thumb reach.
        const towardsContent = root.edge === "bottom" ? Qt.Key_Up : Qt.Key_Down
        if (key === towardsContent) {
            contentRequested()
            return true
        }
        if (key === Qt.Key_Right) {
            focusIndex(focusedIndex() + 1)
            return true
        }
        if (key === Qt.Key_Left) {
            focusIndex(focusedIndex() - 1)
            return true
        }
        return InputKeys.isVertical(key)
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: eventPoint => {
            const remotePoint = castButton.mapFromItem(root, eventPoint.position.x, eventPoint.position.y)
            if (!castButton.contains(remotePoint))
                root.closeRemoteMenu(false)
            const syncPoint = syncButton.mapFromItem(root, eventPoint.position.x, eventPoint.position.y)
            if (!syncButton.contains(syncPoint))
                root.closeSyncPlayMenu(false)
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bgRaised
    }
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: Theme.hoverBorderWidth
        color: Theme.border
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Metrics.scaled(14)
        anchors.rightMargin: Metrics.scaled(14)
        // Butted together, so the hit areas tile with nothing between them.
        // The separation the eye sees is padding inside each button, which
        // does not shrink what can be pressed.
        spacing: 0

        Repeater {
            id: railRepeater
            // Switching user is the provider's sign-in flow, so a source
            // without one has no such button.
            model: {
                const entries = [
                          {
                              label: "My Media",
                              route: "home",
                              icon: "home"
                          },
                          {
                              label: "Search",
                              route: "search",
                              icon: "search"
                          }
                      ]
                if (ProviderCapabilities.auth)
                    entries.push({
                                     label: "Switch user",
                                     route: "switchUser",
                                     icon: "person"
                                 })
                entries.push({
                                 label: "Settings",
                                 route: "settings",
                                 icon: "settings"
                             })
                return entries
            }

            delegate: Item {
                id: railDelegate
                required property var modelData
                readonly property string route: modelData.route
                function forceButtonFocus() {
                    InputKeys.focus(button)
                }
                function hasButtonFocus() {
                    return button.activeFocus
                }
                Layout.preferredWidth: root.railCellWidth
                Layout.fillHeight: true

                IconButton {
                    id: button
                    anchors.centerIn: parent
                    iconName: modelData.icon
                    accessibleName: modelData.route === "switchUser" && root.session
                                    && root.session.activeProfileLabel.length > 0 ? "Switch user — "
                                                                                    + root.session.activeProfileLabel :
                                                                                    modelData.label
                    railStyle: true
                    // The rail changes pages; the selection goes with them.
                    focusOnClick: false
                    selected: root.selectedRoute === modelData.route
                    onClicked: {
                        root.closeSyncPlayMenu(false)
                        root.navigate(modelData.route)
                    }
                }

                Rectangle {
                    anchors.top: button.bottom
                    anchors.topMargin: Metrics.scaled(5)
                    anchors.horizontalCenter: button.horizontalCenter
                    width: switchTooltip.implicitWidth + Metrics.scaled(16)
                    height: switchTooltip.implicitHeight + Metrics.scaled(10)
                    radius: Theme.radiusSmall
                    color: Theme.floatingPanel
                    border.width: Theme.hoverBorderWidth
                    border.color: Theme.borderStrong
                    visible: Platform.hasDesktopPointer && modelData.route === "switchUser" && button.pointerHovered
                    z: 100

                    AppText {
                        id: switchTooltip
                        anchors.centerIn: parent
                        text: "Switch user — " + (root.session ? root.session.activeProfileLabel : "")
                        color: Theme.textPrimary
                        font.pixelSize: Metrics.metaSizePx
                        maximumLineCount: 1
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: true
        }

        Item {
            visible: root.castVisible
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: visible ? root.railCellWidth : 0
            Layout.fillHeight: true

            IconButton {
                id: castButton
                anchors.centerIn: parent
                iconName: root.remoteTargetSelected ? "cast_connected" : "cast"
                accessibleName: root.remoteTargetSelected ? "Play on — " + root.remoteTargetName : "Play on"
                railStyle: true
                selected: root.remoteControlMenuOpen
                onClicked: {
                    if (root.remoteControlMenuOpen)
                        root.closeRemoteMenu(false)
                    else
                        root.openRemoteMenu()
                }

                Rectangle {
                    visible: root.remoteTargetSelected
                    width: Metrics.scaled(9)
                    height: width
                    radius: width / 2
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.rightMargin: Metrics.scaled(7)
                    anchors.topMargin: Metrics.scaled(7)
                    color: Theme.success
                    border.width: Theme.focusBorderWidth
                    border.color: Theme.bgRaised
                }
            }
        }
        Item {
            visible: root.remoteVisible
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: visible ? root.railCellWidth : 0
            Layout.fillHeight: true

            IconButton {
                id: remoteButton
                anchors.centerIn: parent
                iconName: "settings_remote"
                accessibleName: "Remote control — " + root.remoteTargetName
                railStyle: true
                focusOnClick: false
                selected: root.currentRoute === "remoteControl"
                onClicked: {
                    root.closeRemoteMenu(false)
                    root.navigate("remoteControl")
                }

                Rectangle {
                    visible: root.remoteControl ? root.remoteControl.playbackPending : false
                    width: Metrics.scaled(9)
                    height: width
                    radius: width / 2
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.rightMargin: Metrics.scaled(7)
                    anchors.topMargin: Metrics.scaled(7)
                    color: Theme.accent
                    border.width: Theme.focusBorderWidth
                    border.color: Theme.bgRaised
                }
            }
        }

        Item {
            visible: root.syncVisible
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: visible ? root.railCellWidth : 0
            Layout.fillHeight: true

            IconButton {
                id: syncButton
                anchors.centerIn: parent
                iconName: "groups"
                accessibleName: "SyncPlay"
                railStyle: true
                selected: root.syncPlayMenuOpen
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: {
                    if (root.syncPlayMenuOpen)
                        root.closeSyncPlayMenu(false)
                    else
                        root.openSyncMenu()
                }

                // Status dot: accent when in a group, green when groups exist to join.
                Rectangle {
                    visible: root.syncActive || root.syncAvailable
                    width: Metrics.scaled(9)
                    height: width
                    radius: width / 2
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.rightMargin: Metrics.scaled(7)
                    anchors.topMargin: Metrics.scaled(7)
                    color: root.syncActive ? Theme.accent : Theme.success
                    border.width: Theme.focusBorderWidth
                    border.color: Theme.bgRaised
                }
            }
        }
    }

    Loader {
        id: remoteMenuLoader
        width: Metrics.scaled(360)
        anchors.top: parent.bottom
        anchors.right: parent.right
        anchors.topMargin: Metrics.scaled(6)
        anchors.rightMargin: Metrics.scaled(64)
        z: 50
        active: root.remoteControl !== null && root.remoteControlMenuLoaded
        sourceComponent: RemoteControlMenu {
            onRequestClose: root.closeRemoteMenu()
        }
    }

    Loader {
        id: syncMenuLoader
        width: Metrics.scaled(320)
        anchors.top: parent.bottom
        anchors.right: parent.right
        anchors.topMargin: Metrics.scaled(6)
        anchors.rightMargin: Metrics.scaled(14)
        z: 50
        active: root.syncVisible && root.syncPlayMenuLoaded
        sourceComponent: SyncPlayMenu {
            onRequestClose: root.closeSyncPlayMenu()
        }
    }
}
