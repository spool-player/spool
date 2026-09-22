pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

// Horizontal navigation bar: primary routes on the left, watching together on
// the right when an account supports it. D-pad:
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

    readonly property bool groupMenuOpen: groupMenuLoader.item ? groupMenuLoader.item.menuOpen : false
    property bool groupMenuLoaded: false
    readonly property bool groupVisible: ProviderCapabilities.groupPlayback
    readonly property string selectedRoute: currentRoute === "libraryGrid" ? "home" : currentRoute

    // A rail cell is never narrower than the button inside it, so hit areas
    // tile with nothing dead between them on any pointer.
    readonly property int railCellWidth: Math.max(Metrics.scaled(50), Metrics.touchTargetPx)

    // Index space: the rail's buttons, then the group button when shown.
    function lastIndex() {
        return railRepeater.count - (groupVisible ? 0 : 1)
    }

    function focusedIndex() {
        for (let i = 0; i < railRepeater.count; ++i) {
            const item = railRepeater.itemAt(i)
            if (item && item.hasButtonFocus())
                return i
        }
        return groupButton.activeFocus ? railRepeater.count : 0
    }

    function focusIndex(index) {
        const clamped = Math.max(0, Math.min(lastIndex(), index))
        if (clamped === railRepeater.count) {
            InputKeys.focus(groupButton)
            return
        }
        const item = railRepeater.itemAt(clamped)
        if (item)
            item.forceButtonFocus()
    }

    function focusCurrent() {
        for (let i = 0; i < railRepeater.count; ++i) {
            const item = railRepeater.itemAt(i)
            if (item && item.route === selectedRoute) {
                item.forceButtonFocus()
                return
            }
        }
        focusIndex(0)
    }

    function openGroupMenu() {
        if (!groupVisible)
            return
        groupMenuLoaded = true
        if (groupMenuLoader.item)
            groupMenuLoader.item.openMenu()
    }

    function closeGroupMenu(restoreFocus) {
        if (groupMenuLoader.item)
            groupMenuLoader.item.closeMenu()
        if (restoreFocus !== false && groupVisible)
            InputKeys.focus(groupButton)
    }

    function containsGroupPoint(item, x, y) {
        if (groupButton.contains(groupButton.mapFromItem(item, x, y)))
            return true
        const menu = groupMenuLoader.item
        return Boolean(menu && menu.menuOpen && menu.contains(menu.mapFromItem(item, x, y)))
    }

    function activate() {
        const menu = groupMenuLoader.item
        if (menu && menu.menuOpen) {
            menu.activate()
            return
        }
        const index = focusedIndex()
        if (index === railRepeater.count) {
            openGroupMenu()
            return
        }
        const item = railRepeater.itemAt(index)
        if (item)
            navigate(item.route)
    }

    function back() {
        if (!groupMenuOpen)
            return false
        closeGroupMenu()
        return true
    }

    onActiveFocusChanged: if (!activeFocus)
                              closeGroupMenu(false)

    function routeKey(key, phase, repeat) {
        const menu = groupMenuLoader.item
        if (menu && menu.menuOpen)
            return menu.routeKey(key, phase, repeat)
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
            if (!root.containsGroupPoint(root, eventPoint.position.x, eventPoint.position.y))
                root.closeGroupMenu(false)
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
            model: [
                {
                    label: "My Media",
                    route: "home",
                    icon: "home"
                },
                {
                    label: "Search",
                    route: "search",
                    icon: "search"
                },
                {
                    label: "Accounts",
                    route: "accounts",
                    icon: "switch_account"
                },
                {
                    label: "Settings",
                    route: "settings",
                    icon: "settings"
                }
            ]

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
                    accessibleName: modelData.label
                    railStyle: true
                    // The rail changes pages; the selection goes with them.
                    focusOnClick: false
                    selected: root.selectedRoute === modelData.route
                    onClicked: {
                        root.closeGroupMenu(false)
                        root.navigate(modelData.route)
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: true
        }

        Item {
            visible: root.groupVisible
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: visible ? root.railCellWidth : 0
            Layout.fillHeight: true

            IconButton {
                id: groupButton
                anchors.centerIn: parent
                iconName: "groups"
                accessibleName: "Watch together"
                railStyle: true
                selected: root.groupMenuOpen
                onClicked: root.groupMenuOpen ? root.closeGroupMenu(false) : root.openGroupMenu()

                Rectangle {
                    visible: Group.enabled
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
    }

    Loader {
        id: groupMenuLoader
        width: Metrics.scaled(340)
        anchors.top: root.edge === "bottom" ? undefined : parent.bottom
        anchors.bottom: root.edge === "bottom" ? parent.top : undefined
        anchors.right: parent.right
        anchors.topMargin: Metrics.scaled(6)
        anchors.bottomMargin: Metrics.scaled(6)
        anchors.rightMargin: Metrics.scaled(14)
        z: 50
        active: root.groupVisible && root.groupMenuLoaded
        sourceComponent: GroupMenu {
            onRequestClose: root.closeGroupMenu()
        }
        onLoaded: item.openMenu()
    }
}
