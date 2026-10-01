pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Spool

FocusScope {
    id: root
    property var provider
    property Component remoteControls: null
    property string pinLabel: "Account PIN"
    property string pinExplanation: "Cancelling keeps your current account active."
    property var sortChoices: []
    readonly property string kind: provider ? String(provider.arguments.kind || "") : ""
    readonly property bool choosing: kind === "playlist" || kind === "collection"
    readonly property bool naming: choosing || kind === "rename" || kind === "renameCollection"
    property bool busy: false
    property bool exhausted: true
    property string cursor: ""
    property string error: ""
    function loadTargets(append) {
        if (busy)
            return
        busy = true
        provider.requestList("targets", {
                                 kind: kind,
                                 itemId: provider.arguments.itemId,
                                 playlistType: provider.arguments.playlistType || "video",
                                 cursor: append ? cursor : undefined
                             }, append).then(result => {
                                 cursor = String(result.cursor || "")
                                 exhausted = result.exhausted !== false
                                 busy = false
                             }, () => {
                                 busy = false
                                 error = "Couldn't load destinations. Check your connection and permissions."
                             })
    }
    function submitPin() {
        const value = pin.text
        pin.text = ""
        provider.complete({
                              pin: value
                          })
    }
    function activate() {
        const item = Window.activeFocusItem
        if (item && typeof item.activate === "function")
            item.activate()
        else if (item && typeof item.clicked === "function")
            item.clicked()
    }
    Component.onCompleted: {
        if (choosing)
            loadTargets(false)
        if (kind === "rename" || kind === "renameCollection")
            name.text = String(provider.arguments.title || "")
        Qt.callLater(() => kind === "homePin" ? pin.focusRow() : choosing ? InputKeys.focus(list) : naming
                                                                            ? name.focusRow() : InputKeys.focus(
                                                                                  confirm))
    }
    ColumnLayout {
        anchors.fill: root.kind === "homePin" ? undefined : parent
        anchors.centerIn: root.kind === "homePin" ? parent : undefined
        anchors.margins: root.kind === "homePin" ? 0 : Metrics.pageMarginPx
        width: Math.min(parent.width - Metrics.pageMarginPx * 2, Metrics.scaled(520))
        height: root.kind === "homePin" ? implicitHeight : parent.height - Metrics.pageMarginPx * 2
        spacing: Metrics.scaled(12)
        ProviderCompatibilityNotice {
            Layout.fillWidth: true
            provider: root.kind === "homePin" ? null : root.provider
        }
        AppText {
            Layout.fillWidth: true
            text: ({
                       playlist: "Add to playlist",
                       collection: "Add to collection",
                       rename: "Rename",
                       renameCollection: "Rename collection",
                       collectionSort: "Collection order",
                       confirm: "Delete from the server?",
                       remoteControls: "Device controls",
                       homePin: "Unlock " + String(root.provider.arguments.title || "account")
                   })[root.kind] || ""
            font.pixelSize: Metrics.titleSizePx
            font.weight: Font.DemiBold
            wrapMode: Text.WordWrap
        }
        SecondaryText {
            Layout.fillWidth: true
            visible: !!root.error
            text: root.error
            color: Theme.errorText
            wrapMode: Text.WordWrap
        }
        Loader {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: active
            active: root.kind === "remoteControls"
            sourceComponent: root.remoteControls
        }
        TextFieldRow {
            id: pin
            Layout.fillWidth: true
            visible: root.kind === "homePin"
            label: root.pinLabel
            echoMode: TextInput.Password
            inputMethodHints: Qt.ImhDigitsOnly | Qt.ImhNoPredictiveText
            onAccepted: root.submitPin()
        }
        SecondaryText {
            Layout.fillWidth: true
            visible: root.kind === "homePin"
            text: root.pinExplanation
            wrapMode: Text.WordWrap
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.choosing
            clip: true
            model: root.provider ? root.provider.rows : null
            keyNavigationEnabled: true
            delegate: MenuRow {
                required property var record
                required property int index
                width: ListView.view.width
                label: record.title
                iconName: root.kind === "collection" ? "video_library" : "playlist_play"
                highlighted: ListView.isCurrentItem && list.activeFocus
                onHovered: list.currentIndex = index
                onActivated: root.provider.complete({
                                                        targetId: record.id,
                                                        targetName: record.title
                                                    })
            }
            function activate() {
                if (currentItem)
                    currentItem.activated()
            }
        }
        ActionButton {
            visible: root.choosing && !root.exhausted
            enabled: !root.busy
            text: root.busy ? "Loading…" : "Load more"
            onClicked: root.loadTargets(true)
        }
        Repeater {
            model: root.kind === "collectionSort" ? root.sortChoices : []
            delegate: ActionButton {
                required property var modelData
                text: modelData.label
                onClicked: root.provider.complete({
                                                      sort: modelData.value
                                                  })
            }
        }
        TextFieldRow {
            id: name
            Layout.fillWidth: true
            visible: root.naming
            label: root.choosing ? "New " + root.kind : "Name"
            onAccepted: {
                if (text.trim())
                    root.provider.complete({
                                               newName: text.trim()
                                           })
            }
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            ActionButton {
                text: root.kind === "remoteControls" ? "Close" : "Cancel"
                kind: "flat"
                onClicked: root.provider.close()
            }
            ActionButton {
                visible: root.kind === "homePin"
                text: "Unlock"
                kind: "primary"
                onClicked: root.submitPin()
            }
            ActionButton {
                id: confirm
                visible: root.kind === "confirm" || root.naming && !!name.text.trim()
                enabled: !root.busy
                text: root.kind === "confirm" ? "Delete" : root.choosing ? "Create" : "Save"
                kind: root.kind === "confirm" ? "danger" : "primary"
                onClicked: root.provider.complete(root.kind === "confirm" ? {
                                                                                confirmed: true
                                                                            } : {
                                                      newName: name.text.trim()
                                                  })
            }
        }
    }
}
