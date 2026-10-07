import QtQuick
import QtQuick.Layouts
import "../theme"

FocusScope {
    id: root

    property string title: "Confirm"
    property string message: ""
    property string confirmText: "Confirm"
    property bool destructive: false
    property string cancelText: "Cancel"
    // An optional second answer between Cancel and the confirmation.
    property string alternativeText: ""
    // Questions without a risky answer start on the confirmation instead.
    property bool focusConfirm: false

    signal accepted
    signal alternativeChosen
    signal dismissed

    readonly property var buttons: [cancelButton, alternativeButton, confirmButton].filter(button => button.visible)

    anchors.fill: parent
    focus: true
    z: 200

    function routeKey(key, phase, repeat) {
        if (InputKeys.isBack(key, false, false)) {
            if (phase === "release")
                dismissed()
            return true
        }
        if (!InputKeys.isDirection(key))
            return InputKeys.isAccept(key)
        if (phase === "press" && InputKeys.isHorizontal(key)) {
            const index = buttons.findIndex(button => button.activeFocus)
            const step = key === Qt.Key_Left ? -1 : 1
            InputKeys.focus(buttons[Math.max(0, Math.min(buttons.length - 1, index + step))])
        }
        return true
    }

    function activate() {
        if (confirmButton.activeFocus)
            accepted()
        else if (alternativeButton.activeFocus)
            alternativeChosen()
        else
            dismissed()
    }

    function back() {
        dismissed()
        return true
    }

    Component.onCompleted: Qt.callLater(function () {
        InputKeys.focus(root.focusConfirm ? confirmButton : cancelButton)
    })

    Rectangle {
        anchors.fill: parent
        color: "#99000000"

        TapHandler {
            onTapped: root.dismissed()
        }
    }

    Surface {
        anchors.centerIn: parent
        width: Math.min(parent.width - Metrics.scaled(96), Metrics.scaled(560))
        height: content.implicitHeight + Metrics.scaled(48)
        elevated: true
        baseColor: Theme.floatingPanel

        ColumnLayout {
            id: content
            anchors.fill: parent
            anchors.margins: Metrics.scaled(24)
            spacing: Metrics.scaled(16)

            AppText {
                Layout.fillWidth: true
                text: root.title
                font.pixelSize: Metrics.titleSizePx
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
            }

            AppText {
                Layout.fillWidth: true
                text: root.message
                color: Theme.textSecondary
                font.pixelSize: Metrics.bodySizePx
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Metrics.scaled(8)
                spacing: Metrics.scaled(12)

                Item {
                    Layout.fillWidth: true
                }

                ActionButton {
                    id: cancelButton
                    text: root.cancelText
                    onClicked: root.dismissed()
                }

                ActionButton {
                    id: alternativeButton
                    visible: root.alternativeText.length > 0
                    text: root.alternativeText
                    onClicked: root.alternativeChosen()
                }

                ActionButton {
                    id: confirmButton
                    text: root.confirmText
                    kind: root.destructive ? "danger" : "primary"
                    onClicked: root.accepted()
                }
            }
        }
    }
}
