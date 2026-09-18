pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../../theme"
import "../../primitives"

FocusScope {
    id: menu

    property bool menuOpen: false
    property var entries: []
    property int currentIndex: 0
    signal requestClose

    visible: menuOpen
    implicitHeight: panel.implicitHeight
    height: implicitHeight

    function actionable(entry) {
        return entry && entry.kind !== "empty" && entry.kind !== "status"
    }

    function buildEntries() {
        const out = []
        if (RemoteControl.targetSelected) {
            out.push({
                         kind: "status",
                         label: "Controlling " + (RemoteControl.selectedTargetName || "selected device"),
                         detail: RemoteControl.selectedTargetDetail || "Connected",
                         icon: "cast_connected"
                     })
            out.push({
                         kind: "disconnect",
                         label: "Stop controlling",
                         detail: "Playback on the target continues",
                         icon: "cast"
                     })
        }
        const targets = RemoteControl.targets || []
        for (let index = 0; index < targets.length; ++index) {
            const target = targets[index] || ({})
            if (target.sessionId === RemoteControl.selectedSessionId)
                continue
            const detail = []
            if (target.client)
                detail.push(target.client)
            if (target.userName)
                detail.push(target.userName)
            if (target.nowPlayingTitle)
                detail.push("Playing " + target.nowPlayingTitle)
            out.push({
                         kind: "target",
                         sessionId: target.sessionId || "",
                         label: target.deviceName || target.client || "Jellyfin client",
                         detail: detail.join(" · "),
                         icon: target.deviceType === "TV" ? "tv" : "devices"
                     })
        }
        if (out.length === 0)
            out.push({
                         kind: "empty",
                         label: RemoteControl.busy ? "Looking for clients…" : "No remote clients available",
                         detail: "Enable remote control on the target device",
                         icon: RemoteControl.busy ? "sync" : "cast"
                     })
        out.push({
                     kind: "refresh",
                     label: "Refresh clients",
                     detail: "",
                     icon: "refresh"
                 })
        return out
    }

    function firstActionable() {
        for (let index = 0; index < entries.length; ++index)
            if (actionable(entries[index]))
                return index
        return 0
    }

    function openMenu() {
        RemoteControl.refreshTargets()
        entries = buildEntries()
        currentIndex = firstActionable()
        menuOpen = true
        Qt.callLater(function () {
            list.currentIndex = currentIndex
            InputKeys.focus(list)
        })
    }

    function closeMenu() {
        menuOpen = false
    }

    function activateEntry(index) {
        const entry = entries[index]
        if (!actionable(entry))
            return
        if (entry.kind === "target") {
            RemoteControl.selectTarget(entry.sessionId)
        } else if (entry.kind === "disconnect") {
            RemoteControl.clearTarget()
        } else if (entry.kind === "refresh") {
            RemoteControl.refreshTargets()
            return
        }
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
        target: RemoteControl
        enabled: menu.menuOpen
        function onTargetsChanged() {
            menu.entries = menu.buildEntries()
            menu.currentIndex = Math.min(menu.currentIndex, Math.max(0, menu.entries.length - 1))
        }
        function onTargetChanged() {
            menu.entries = menu.buildEntries()
            menu.currentIndex = menu.firstActionable()
        }
        function onBusyChanged() {
            menu.entries = menu.buildEntries()
        }
    }

    Surface {
        id: panel
        anchors.fill: parent
        implicitHeight: Math.min(Metrics.scaled(520), contentColumn.implicitHeight + Metrics.scaled(20))
        elevated: true
        baseColor: Theme.bgRaised
        clip: true

        PopupShield {}

        ColumnLayout {
            id: contentColumn
            anchors.fill: parent
            anchors.margins: Metrics.scaled(10)
            spacing: Metrics.scaled(6)

            AppText {
                Layout.fillWidth: true
                Layout.leftMargin: Metrics.scaled(4)
                text: "Play On"
                color: Theme.textMuted
                font.pixelSize: Metrics.metaSizePx
                font.weight: Font.DemiBold
            }

            MenuListView {
                id: list
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, Metrics.scaled(470))
                model: menu.entries
                currentIndex: menu.currentIndex
                rowEnabled: function (entry, index) {
                    return menu.actionable(menu.entries[index])
                }
                onCurrentIndexChanged: menu.currentIndex = currentIndex
                onDismissed: menu.requestClose()
                onAccepted: index => menu.activateEntry(index)

                delegate: MenuRow {
                    required property int index
                    required property var modelData
                    width: list.width
                    label: modelData.label || ""
                    detail: modelData.detail || ""
                    iconName: modelData.icon || ""
                    actionable: menu.actionable(modelData)
                    highlighted: ListView.isCurrentItem && list.activeFocus
                    checkIconName: ""
                    onHovered: list.currentIndex = index
                    onActivated: menu.activateEntry(index)
                }
            }
        }
    }
}
