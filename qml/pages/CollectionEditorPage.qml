pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

FocusScope {
    id: root
    property var shell
    readonly property bool contentReady: !CollectionEditing.busy || CollectionEditing.entries.length > 0
    property int actionIndex: -1
    property int pendingIndex: -1
    readonly property var actions: {
        const result = []
        if (CollectionEditing.movable) {
            result.push({
                            id: "up",
                            label: "Move up",
                            icon: "arrow_upward"
                        })
            result.push({
                            id: "down",
                            label: "Move down",
                            icon: "arrow_downward"
                        })
        }
        if (CollectionEditing.removable)
            result.push({
                            id: "remove",
                            label: "Remove",
                            icon: "remove_circle_outline"
                        })
        result.push({
                        id: "refresh",
                        label: "Refresh",
                        icon: "refresh"
                    })
        return result
    }
    onActionsChanged: actionIndex = Math.min(actionIndex, actions.length - 1)

    function enter() {
        if (!visible || Router.route !== "collectionEditor")
            return
        if (CollectionEditing.containerId === String(Router.args.containerId || ""))
            return
        actionIndex = -1
        pendingIndex = -1
        CollectionEditing.open(String(Router.args.containerId || ""), String(Router.args.title || "Collection"))
        InputKeys.focus(root)
    }
    Component.onCompleted: enter()
    onVisibleChanged: {
        if (visible)
            enter()
        else
            CollectionEditing.close()
    }
    Component.onDestruction: {
        if (Router.route === "collectionEditor")
            CollectionEditing.close()
    }
    Connections {
        target: CollectionEditing
        function onChanged() {
            if (root.pendingIndex >= 0 && !CollectionEditing.busy) {
                const index = Math.min(root.pendingIndex, CollectionEditing.entries.length - 1)
                root.pendingIndex = -1
                if (index >= 0)
                    CollectionEditing.selectEntry(CollectionEditing.entries[index].entryId)
            }
            if (CollectionEditing.selectedIndex >= 0)
                list.positionViewAtIndex(CollectionEditing.selectedIndex, ListView.Contain)
        }
    }

    function actionEnabled(id) {
        if (CollectionEditing.busy)
            return false
        const index = CollectionEditing.selectedIndex
        if (id === "refresh")
            return CollectionEditing.containerId.length > 0
        if (index < 0)
            return false
        if (id === "up")
            return index > 0
        if (id === "down")
            return index + 1 < CollectionEditing.entries.length || CollectionEditing.hasMore
        return true
    }
    function perform(id) {
        if (!actionEnabled(id))
            return
        const entry = CollectionEditing.selectedEntryId
        if (id === "up" || id === "down")
            CollectionEditing.moveEntry(entry, id === "up" ? -1 : 1)
        else if (id === "remove")
            CollectionEditing.removeEntry(entry)
        else
            CollectionEditing.refresh()
    }
    function activate() {
        if (actionIndex < 0)
            actionIndex = 0
        else
            perform(actions[actionIndex].id)
    }
    function back() {
        if (actionIndex < 0)
            return false
        actionIndex = -1
        return true
    }
    function routeKey(key, phase, repeat) {
        if (phase === "release")
            return InputKeys.isDirection(key)
        if (key === Qt.Key_Left || key === Qt.Key_Right) {
            actionIndex = Math.max(-1, Math.min(actions.length - 1, actionIndex + (key === Qt.Key_Right ? 1 : -1)))
            return true
        }
        if (key === Qt.Key_Up || key === Qt.Key_Down) {
            actionIndex = -1
            const index = CollectionEditing.selectedIndex + (key === Qt.Key_Down ? 1 : -1)
            if (index >= 0 && index < CollectionEditing.entries.length)
                CollectionEditing.selectEntry(CollectionEditing.entries[index].entryId)
            else if (key === Qt.Key_Down && CollectionEditing.hasMore && !CollectionEditing.busy) {
                pendingIndex = index
                CollectionEditing.requestNextPage()
            } else if (key === Qt.Key_Up && index < 0 && shell)
                shell.focusNavBar()
            return true
        }
        return false
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Metrics.pageMarginPx
        spacing: Metrics.gapPx
        AppText {
            Layout.fillWidth: true
            text: CollectionEditing.title || "Collection entries"
            font.pixelSize: Metrics.titleSizePx
            elide: Text.ElideRight
        }
        AppText {
            Layout.fillWidth: true
            text: CollectionEditing.problem || (CollectionEditing.busy ? "Loading…" : CollectionEditing.entries.length
                                                                         === 0 ? "No entries." :
                                                                                 "Select an entry. Right opens its actions; Up and Down select entries.")
            color: Theme.textSecondary
            font.pixelSize: Metrics.bodySizePx
            wrapMode: Text.WordWrap
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: CollectionEditing.entries
            currentIndex: CollectionEditing.selectedIndex
            boundsBehavior: Flickable.StopAtBounds
            delegate: MenuRow {
                required property int index
                required property var modelData
                // Media IDs may repeat; this identity is the exact occurrence.
                readonly property string entryId: modelData.entryId
                width: list.width
                label: String(index + 1) + ". " + modelData.title
                highlighted: entryId === CollectionEditing.selectedEntryId
                onActivated: {
                    CollectionEditing.selectEntry(entryId)
                    root.actionIndex = 0
                    InputKeys.focus(root)
                }
                Accessible.name: label
                Accessible.selected: entryId === CollectionEditing.selectedEntryId
            }
            footer: MenuRow {
                width: list.width
                visible: CollectionEditing.hasMore
                height: visible ? Math.max(Metrics.touchTargetPx, Metrics.scaled(46)) : 0
                label: CollectionEditing.busy ? "Loading…" : "Load more entries"
                iconName: "expand_more"
                actionable: !CollectionEditing.busy
                onActivated: CollectionEditing.requestNextPage()
            }
            FastWheelHandler {
                flickable: list
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Metrics.gapPx
            Repeater {
                model: root.actions
                delegate: MenuRow {
                    required property int index
                    required property var modelData
                    Layout.fillWidth: true
                    label: modelData.label
                    iconName: modelData.icon
                    highlighted: root.actionIndex === index
                    actionable: root.actionEnabled(modelData.id)
                    onActivated: {
                        root.actionIndex = index
                        root.perform(modelData.id)
                        InputKeys.focus(root)
                    }
                    Accessible.role: Accessible.Button
                    Accessible.name: label
                    Accessible.onPressAction: root.perform(modelData.id)
                }
            }
        }
    }
}
