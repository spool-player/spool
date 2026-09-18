pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../../theme"
import "../../primitives"

// The three things you can do to a saved account: sign in again, correct the
// server it points at, or forget it. They share one anchor and one key route,
// so the picker only has to open and close this.
Item {
    id: root

    property string profileId: ""
    property string serverName: ""
    property string serverUrl: ""
    property Item anchorItem: null

    // "" | "menu" | "edit" | "remove"
    property string mode: ""
    readonly property bool open: mode.length > 0

    signal signInAgainRequested
    signal closed

    function show(id, anchor, name, url) {
        profileId = id
        anchorItem = anchor
        serverName = name
        serverUrl = url
        mode = "menu"
    }

    function close() {
        mode = ""
        profileId = ""
        anchorItem = null
        root.closed()
    }

    function routeKey(key, phase, repeat) {
        return loader.item ? loader.item.routeKey(key, phase, repeat) : false
    }

    function activate() {
        if (loader.item)
            loader.item.activate()
    }

    function back() {
        if (!open)
            return false
        close()
        return true
    }

    Loader {
        id: loader
        anchors.fill: parent
        active: root.open
        sourceComponent: root.mode === "menu" ? actionMenu : root.mode === "remove" ? removeDialog : editDialog
    }

    Component {
        id: actionMenu

        OptionPickerDialog {
            title: "Account actions"
            options: ["Sign in again", "Edit server", "Remove from this device"]
            currentIndex: 0
            anchorItem: root.anchorItem
            onSelected: index => {
                if (index === 0) {
                    Session.prepareProfileSignIn(root.profileId)
                    root.close()
                    root.signInAgainRequested()
                    return
                }
                root.mode = index === 1 ? "edit" : "remove"
            }
            onDismissed: root.close()
        }
    }

    Component {
        id: removeDialog

        ConfirmationDialog {
            title: "Remove " + root.serverName + "?"
            message: "This removes the saved account and token for " + root.serverName
                     + " on this device. Other accounts are unchanged."
            confirmText: "Remove"
            destructive: true
            onAccepted: {
                Session.removeProfile(root.profileId)
                root.close()
            }
            onDismissed: root.close()
        }
    }

    Component {
        id: editDialog

        FocusScope {
            id: editScope

            property string nameDraft: root.serverName
            property string urlDraft: root.serverUrl
            readonly property var controls: [nameField, addressField, cancelButton, saveButton]

            anchors.fill: parent
            focus: true

            function focusControl(item) {
                if (item === nameField || item === addressField)
                    item.focusRow()
                else
                    InputKeys.focus(item)
            }

            function routeKey(key, phase, repeat) {
                if (InputKeys.isBack(key, false, false)) {
                    if (phase === "release")
                        root.close()
                    return true
                }
                if (!InputKeys.isDirection(key))
                    return InputKeys.isAccept(key)
                if (phase !== "press")
                    return true

                let index = controls.findIndex(item => item.activeFocus)
                if (index < 0)
                    index = 0
                if (key === Qt.Key_Down || key === Qt.Key_Right)
                    index = Math.min(controls.length - 1, index + 1)
                else
                    index = Math.max(0, index - 1)
                focusControl(controls[index])
                return true
            }

            function activate() {
                if (saveButton.activeFocus)
                    saveButton.clicked()
                else if (cancelButton.activeFocus)
                    root.close()
            }

            Component.onCompleted: Qt.callLater(function () {
                nameField.focusRow()
            })

            Rectangle {
                anchors.fill: parent
                color: Theme.overlayScrimStrong
            }

            Surface {
                anchors.centerIn: parent
                width: Math.min(parent.width - Metrics.scaled(96), Metrics.scaled(620))
                height: editContent.implicitHeight + Metrics.scaled(48)
                elevated: true
                baseColor: Theme.floatingPanel

                ColumnLayout {
                    id: editContent
                    anchors.fill: parent
                    anchors.margins: Metrics.scaled(24)
                    spacing: Metrics.scaled(14)

                    AppText {
                        Layout.fillWidth: true
                        text: "Edit server"
                        font.pixelSize: Metrics.titleSizePx
                        font.weight: Font.DemiBold
                    }

                    TextFieldRow {
                        id: nameField
                        Layout.fillWidth: true
                        label: "Server label"
                        text: editScope.nameDraft
                        onTextEdited: text => editScope.nameDraft = text
                    }

                    TextFieldRow {
                        id: addressField
                        Layout.fillWidth: true
                        label: "Server address"
                        text: editScope.urlDraft
                        inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                        onTextEdited: text => editScope.urlDraft = text
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Metrics.scaled(12)

                        Item {
                            Layout.fillWidth: true
                        }

                        ActionButton {
                            id: cancelButton
                            text: "Cancel"
                            onClicked: root.close()
                        }

                        ActionButton {
                            id: saveButton
                            text: "Save"
                            kind: "primary"
                            onClicked: {
                                Session.updateProfileServer(root.profileId, editScope.nameDraft, editScope.urlDraft)
                                root.close()
                            }
                        }
                    }
                }
            }
        }
    }
}
