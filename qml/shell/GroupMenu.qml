pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

// The top bar's watch-together menu. In a group it shows who is in it and a
// way out; otherwise the groups every account can see, and a way to start one.
FocusScope {
    id: menu

    property bool menuOpen: false
    property var entries: []
    property int currentIndex: 0
    signal requestClose

    visible: menuOpen
    implicitHeight: panel.implicitHeight
    height: implicitHeight

    function isActionable(entry) {
        return entry && (entry.kind === "join" || entry.kind === "create" || entry.kind === "leave")
    }

    function firstActionable(list) {
        for (let i = 0; i < list.length; ++i)
            if (isActionable(list[i]))
                return i
        return 0
    }

    function people(count) {
        return count + (count === 1 ? " person" : " people")
    }

    function buildEntries() {
        const out = []
        if (Group.enabled) {
            const parts = [people(Group.participantCount)]
            if (Group.groupState.length > 0)
                parts.push(Group.groupState)
            if (Group.pingMs > 0)
                parts.push(Math.round(Group.pingMs) + " ms")
            out.push({
                         kind: "current",
                         label: Group.currentGroupName,
                         sub: parts.join(" · "),
                         icon: "groups"
                     })
            const names = Group.participants.join(", ")
            if (names.length > 0)
                out.push({
                             kind: "info",
                             label: names,
                             sub: "",
                             icon: "person"
                         })
            out.push({
                         kind: "leave",
                         label: "Leave",
                         sub: "",
                         icon: "logout"
                     })
            return out
        }
        const groups = Group.groups || []
        const accounts = {}
        for (const group of groups) {
            accounts[group.accountId] = group.accountLabel
            out.push({
                         kind: "join",
                         groupId: group.id,
                         label: group.name || "Group",
                         sub: people((group.participants || []).length) + " · " + group.accountLabel,
                         icon: "login"
                     })
        }
        if (groups.length === 0)
            out.push({
                         kind: "info",
                         label: Group.loadingGroups ? "Looking for groups…" : "No groups yet",
                         sub: "",
                         icon: Group.loadingGroups ? "hourglass_empty" : "block"
                     })
        for (const source of Providers.accounts) {
            if (!source.running || !source.enabled)
                continue
            out.push({
                         kind: "create",
                         accountId: source.id,
                         label: "New group",
                         sub: source.label + " · " + source.providerName,
                         icon: "add"
                     })
        }
        return out
    }

    function openMenu() {
        Group.refreshGroups()
        entries = buildEntries()
        currentIndex = firstActionable(entries)
        menuOpen = true
        Qt.callLater(function () {
            if (!menuOpen)
                return
            list.currentIndex = currentIndex
            InputKeys.focus(list)
        })
    }

    function closeMenu() {
        menuOpen = false
    }

    function activateEntry(index) {
        const entry = entries[index]
        if (!entry)
            return
        if (entry.kind === "join")
            Group.joinGroup(entry.groupId)
        else if (entry.kind === "create")
            Group.createGroup(entry.accountId, "Watch together")
        else if (entry.kind === "leave")
            Group.leaveGroup()
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
        target: Group
        enabled: menu.menuOpen
        function onGroupsChanged() {
            menu.entries = menu.buildEntries()
            menu.currentIndex = Math.min(menu.currentIndex, Math.max(0, menu.entries.length - 1))
        }
        function onGroupChanged() {
            menu.entries = menu.buildEntries()
            menu.currentIndex = menu.firstActionable(menu.entries)
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
                text: "Watch together"
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
                    width: list.width
                    label: modelData.label || ""
                    detail: modelData.sub || ""
                    iconName: modelData.icon || ""
                    actionable: menu.isActionable(modelData)
                    highlighted: ListView.isCurrentItem && list.activeFocus
                    checkIconName: ""
                    onHovered: list.currentIndex = index
                    onActivated: menu.activateEntry(index)
                }
            }
        }
    }
}
