pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../../theme"
import "../../primitives"

// Dropdown menu for the top-bar SyncPlay button. Mirrors jellyfin-web's
// SyncPlayMenu: when in a group it shows the active group and a Leave action;
// otherwise it lists joinable groups and an option to create a new one.
FocusScope {
    id: menu

    property bool menuOpen: false
    readonly property var syncPlay: SyncPlay
    property var entries: []
    property int currentIndex: 0
    signal requestClose

    visible: menuOpen
    implicitHeight: panel.implicitHeight
    height: implicitHeight

    function defaultGroupName() {
        const name = Session.username ? String(Session.username) : ""
        return (name.length > 0 ? name : "My") + "'s group"
    }

    function isActionable(entry) {
        return entry && (entry.kind === "join" || entry.kind === "create" || entry.kind === "leave")
    }

    function firstActionable(list) {
        for (let i = 0; i < list.length; ++i)
            if (isActionable(list[i]))
                return i
        return 0
    }

    function buildEntries() {
        const out = []
        if (!syncPlay)
            return out
        if (syncPlay.enabled) {
            // The server reports distinct usernames, not sessions, so several
            // devices signed in as one person count once.
            const memberCount = syncPlay.participantCount || 0
            const statusParts = [memberCount + " user" + (memberCount === 1 ? "" : "s")]
            if ((syncPlay.groupState || "").length > 0)
                statusParts.push(syncPlay.groupState)
            statusParts.push(syncPlay.socketConnected ? Math.round(syncPlay.pingMs) + " ms" : "Reconnecting")
            out.push({
                         kind: "current",
                         label: syncPlay.currentGroupName || "SyncPlay group",
                         sub: statusParts.join(" / "),
                         icon: "groups"
                     })
            out.push({
                         kind: "leave",
                         label: "Leave group",
                         sub: "",
                         icon: "logout"
                     })
        } else {
            const groups = syncPlay.groups || []
            for (let i = 0; i < groups.length; ++i) {
                const group = groups[i] || ({})
                const count = group.Participants ? group.Participants.length : 0
                out.push({
                             kind: "join",
                             groupId: group.GroupId || "",
                             label: group.GroupName || "Group",
                             sub: count + " user" + (count === 1 ? "" : "s"),
                             icon: "login"
                         })
            }
            if (groups.length === 0)
                out.push({
                             kind: "empty",
                             label: "No groups available",
                             sub: "",
                             icon: "block"
                         })
            out.push({
                         kind: "create",
                         label: "Create a new group",
                         sub: "",
                         icon: "add"
                     })
        }
        return out
    }

    function openMenu() {
        entries = buildEntries()
        currentIndex = firstActionable(entries)
        menuOpen = true
        Qt.callLater(function () {
            if (!menuOpen)
                return
            list.currentIndex = currentIndex
            InputKeys.focus(list)
            list.forceActiveFocus(Qt.PopupFocusReason)
        })
    }

    function closeMenu() {
        menuOpen = false
    }

    function activateEntry(index) {
        const entry = entries[index]
        if (!entry || !syncPlay)
            return
        if (entry.kind === "join") {
            syncPlay.joinGroup(entry.groupId)
            // The device that is playing is the one the group has to contain,
            // so the target joins on its own behalf rather than being driven
            // from here.
            if (RemoteControl.targetSelected)
                RemoteControl.requestSyncPlayJoin(entry.groupId)
        } else if (entry.kind === "create")
            syncPlay.createGroup(defaultGroupName())
        else if (entry.kind === "leave")
            syncPlay.leaveGroup()
        else
            return
        requestClose()
    }

    function routeKey(key, phase, repeat) {
        return list.routeKey(key, phase, repeat)
    }

    function activate() {
        list.activate()
    }

    function back() {
        requestClose()
        return true
    }

    Connections {
        target: menu.syncPlay
        enabled: menu.menuOpen
        function onGroupsChanged() {
            menu.entries = menu.buildEntries()
            menu.currentIndex = Math.min(menu.currentIndex, Math.max(0, menu.entries.length - 1))
        }
        function onGroupChanged() {
            menu.entries = menu.buildEntries()
            menu.currentIndex = menu.firstActionable(menu.entries)
        }
        function onConnectionChanged() {
            menu.entries = menu.buildEntries()
        }
    }

    Surface {
        id: panel
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        implicitHeight: contentColumn.implicitHeight + 20
        height: implicitHeight
        elevated: true
        baseColor: Theme.bgRaised
        clip: true

        PopupShield {}

        ColumnLayout {
            id: contentColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 10
            spacing: 6

            AppText {
                Layout.fillWidth: true
                Layout.leftMargin: 4
                text: "SyncPlay"
                color: Theme.textMuted
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }

            MenuListView {
                id: list
                Layout.fillWidth: true
                Layout.preferredHeight: contentHeight
                interactive: false
                model: menu.entries
                currentIndex: menu.currentIndex
                rowEnabled: function (entry, index) {
                    return menu.isActionable(menu.entries[index])
                }
                onCurrentIndexChanged: menu.currentIndex = currentIndex
                onDismissed: menu.requestClose()
                onAccepted: index => menu.activateEntry(index)

                delegate: MenuRow {
                    required property int index
                    required property var modelData
                    readonly property bool rowActionable: menu.isActionable(modelData)
                    width: list.width
                    label: modelData.label || ""
                    detail: modelData.sub || ""
                    iconName: modelData.icon || ""
                    actionable: rowActionable
                    highlighted: ListView.isCurrentItem && list.activeFocus
                    checkIconName: ""
                    onHovered: list.currentIndex = index
                    onActivated: menu.activateEntry(index)
                }
            }
        }
    }
}
