pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../../theme"
import "../../primitives"
import "../../shell"

OverlayDialog {
    id: root

    property string mode: ""
    property var item: ({})
    property string nameDraft: ""
    property int targetIndex: 0
    readonly property var targets: mode === "collection" ? Management.collectionTargets : Management.playlistTargets
    readonly property string heading: mode === "playlist" ? "Add to playlist" : mode === "collection"
                                                            ? "Add to collection" : mode === "rename" ? "Rename" : mode
                                                                                                        === "delete"
                                                                                                        ? "Delete item" :
                                                                                                          Browse.viewKind
                                                                                                          === "playlist"
                                                                                                          ? "Remove from playlist" :
                                                                                                            "Remove from collection"

    function itemTitle(value) {
        return String(value && (value.displayTitle || value.title || value.seriesName || value.name) || "Selected item")
    }

    function focusNameField() {
        InputKeys.focus(managementNameField)
        managementNameField.focusField()
    }

    function prepare() {
        targetIndex = 0
        nameDraft = mode === "rename" ? itemTitle(item) : ""
        if (mode === "playlist" || mode === "collection")
            Management.refreshTargets(mode)
        if (mode === "rename")
            Qt.callLater(focusNameField)
        else
            Qt.callLater(function () {
                InputKeys.focus(root)
            })
    }

    function submitName(name) {
        const value = String(name || "").trim()
        if (value.length === 0)
            return
        if (mode === "playlist")
            Management.createPlaylistForItem(value, item)
        else if (mode === "collection")
            Management.createCollectionForItem(value, item)
        else
            Management.renameItem(item, value)
        dismissed()
    }

    function selectTarget(index) {
        const target = targets[index] || ({})
        const targetId = String(target.movieId || target.id || "")
        if (targetId.length === 0)
            return
        if (mode === "playlist")
            Management.addItemToPlaylist(targetId, item)
        else
            Management.addItemToCollection(targetId, item)
        dismissed()
    }

    function confirm() {
        if (mode === "delete")
            Management.deleteItem(item)
        else
            Management.removeItemFromCurrentParent(item)
        dismissed()
    }

    function routeKey(key, phase, repeat) {
        if (mode !== "playlist" && mode !== "collection")
            return false
        if (key === Qt.Key_Up && targetIndex === 0) {
            focusNameField()
            return true
        }
        targetIndex = Math.max(0, Math.min(targets.length - 1, targetIndex + (key === Qt.Key_Down ? 1 : key
                                                                                                    === Qt.Key_Up ? -1 :
                                                                                                                    0)))
        return InputKeys.isHorizontal(key) || key === Qt.Key_Up || key === Qt.Key_Down
    }

    function activate() {
        if (mode === "playlist" || mode === "collection")
            selectTarget(targetIndex)
        else if (mode === "rename")
            submitName(nameDraft)
        else
            confirm()
    }

    function back() {
        dismissed()
        return true
    }

    AppText {
        Layout.fillWidth: true
        text: root.heading
        color: Theme.textPrimary
        font.pixelSize: Metrics.titleSizePx
        font.weight: Font.DemiBold
    }

    AppText {
        Layout.fillWidth: true
        text: root.itemTitle(root.item)
        color: Theme.textMuted
        font.pixelSize: Metrics.bodySizePx
        elide: Text.ElideRight
        visible: root.mode.length > 0
    }

    TextFieldRow {
        id: managementNameField
        Layout.fillWidth: true
        visible: root.mode === "playlist" || root.mode === "collection" || root.mode === "rename"
        label: root.mode === "rename" ? "Name" : (root.mode === "collection" ? "New collection name" :
                                                                               "New playlist name")
        text: root.nameDraft
        onTextEdited: value => root.nameDraft = value
        onAccepted: root.submitName(root.nameDraft)
    }

    AppText {
        Layout.fillWidth: true
        visible: root.mode === "delete" || root.mode === "remove"
        text: root.mode === "delete" ? "This permanently deletes the item from the server." :
                                       "This removes the item from the current playlist or collection."
        color: Theme.textMuted
        font.pixelSize: Metrics.bodySizePx
        wrapMode: Text.Wrap
    }

    Repeater {
        model: root.mode === "playlist" || root.mode === "collection" ? root.targets.length : 0
        delegate: Rectangle {
            required property int index
            Layout.fillWidth: true
            height: Metrics.controlHeightPx
            radius: Theme.radiusMedium
            color: root.targetIndex === index ? Theme.focusedFill : "transparent"
            border.width: root.targetIndex === index ? Theme.focusBorderWidth : Theme.hoverBorderWidth
            border.color: root.targetIndex === index ? Theme.accent : Theme.border

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Metrics.scaled(14)
                anchors.rightMargin: Metrics.scaled(14)
                spacing: Metrics.scaled(12)
                MaterialIcon {
                    name: root.mode === "collection" ? "collections_bookmark" : "playlist_play"
                    iconColor: Theme.textPrimary
                    iconSize: Metrics.iconSizePx
                }
                AppText {
                    Layout.fillWidth: true
                    text: root.itemTitle(root.targets[index] || ({}))
                    color: Theme.textPrimary
                    font.pixelSize: Metrics.bodySizePx
                    elide: Text.ElideRight
                }
            }

            MouseArea {
                anchors.fill: parent
                onClicked: root.selectTarget(index)
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        visible: root.mode === "delete" || root.mode === "remove"
        Item {
            Layout.fillWidth: true
        }
        ActionButton {
            text: "Cancel"
            onClicked: root.dismissed()
        }
        ActionButton {
            text: root.mode === "delete" ? "Delete" : "Remove"
            kind: "primary"
            onClicked: root.confirm()
        }
    }
}
