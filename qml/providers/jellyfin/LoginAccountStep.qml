import QtQuick
import QtQuick.Layouts
import "../../theme"
import "../../primitives"

// Signing in to the server the previous step settled on.
//
// The server it is about stays on screen as the same row that chose it, and
// stays reachable, because getting it wrong is the failure this form actually
// has: a password rejected by the wrong machine reads exactly like a password
// typed wrong.
FocusScope {
    id: root

    property bool dense: false
    property string serverName: ""
    property string serverAddress: ""

    signal changeServerRequested
    signal signInRequested

    function controls() {
        return [serverRow, usernameRow, passwordRow, signInButton, quickConnectButton]
    }

    function focusControl(item) {
        if (item === usernameRow || item === passwordRow)
            item.focusRow()
        else
            InputKeys.focus(item)
    }

    function focusDefault() {
        usernameRow.focusField()
    }

    function moveInside(control, key) {
        return false
    }

    function activateControl(control) {
        if (control === serverRow)
            root.changeServerRequested()
        else if (control === usernameRow || control === passwordRow)
            control.focusField()
        else if (control === quickConnectButton)
            quickConnectButton.clicked()
        else
            root.signInRequested()
    }

    ColumnLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: Metrics.scaled(10)

        AppText {
            Layout.fillWidth: true
            Layout.bottomMargin: Metrics.scaled(8)
            visible: !root.dense
            text: "Sign in"
            font.pixelSize: Metrics.titleSizePx
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }

        ServerCard {
            id: serverRow
            Layout.fillWidth: true
            Layout.bottomMargin: Metrics.scaled(6)
            title: root.serverName
            serverAddress: root.serverAddress
            status: "Change"
            onAccepted: {
                InputKeys.focus(serverRow)
                root.changeServerRequested()
            }
        }

        AppText {
            Layout.fillWidth: true
            Layout.topMargin: Metrics.scaled(4)
            text: "Username"
            font.pixelSize: Metrics.bodySizePx
            font.weight: Font.Medium
        }

        TextFieldRow {
            id: usernameRow
            Layout.fillWidth: true
            accessibleName: "Username"
            text: Session.username
            inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
            enterKeyType: Qt.EnterKeyNext
            onTextEdited: text => Session.username = text
            onAccepted: passwordRow.focusField()
        }

        AppText {
            Layout.fillWidth: true
            Layout.topMargin: Metrics.scaled(4)
            text: "Password"
            font.pixelSize: Metrics.bodySizePx
            font.weight: Font.Medium
        }

        TextFieldRow {
            id: passwordRow
            Layout.fillWidth: true
            Layout.bottomMargin: Metrics.scaled(10)
            accessibleName: "Password"
            text: Session.password
            echoMode: TextInput.Password
            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
            enterKeyType: Qt.EnterKeyGo
            onTextEdited: text => Session.password = text
            onAccepted: root.signInRequested()
        }

        ActionButton {
            id: signInButton
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(Metrics.controlHeightPx, Metrics.scaled(56))
            text: "Sign in"
            iconName: "login"
            kind: "primary"
            onClicked: root.signInRequested()
        }

        // Quick Connect is the other way in, not a second half of this one, so
        // it sits below the rule rather than beside the primary button.
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Metrics.scaled(6)
            Layout.bottomMargin: Metrics.scaled(6)
            visible: !root.dense
            spacing: Metrics.scaled(12)

            Rectangle {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredHeight: Math.max(1, Metrics.scaled(1))
                color: Theme.border
            }

            SecondaryText {
                text: "or"
                color: Theme.textMuted
                font.pixelSize: Metrics.metaSizePx
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredHeight: Math.max(1, Metrics.scaled(1))
                color: Theme.border
            }
        }

        ActionButton {
            id: quickConnectButton
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(Metrics.controlHeightPx, Metrics.scaled(52))
            text: QuickConnect.active ? "Cancel Quick Connect" : "Use Quick Connect"
            iconName: QuickConnect.active ? "close" : "bolt"
            onClicked: {
                if (QuickConnect.active) {
                    QuickConnect.cancel()
                } else {
                    App.clearError()
                    QuickConnect.start(Session.serverUrl)
                }
            }
        }

        Surface {
            Layout.fillWidth: true
            Layout.topMargin: Metrics.scaled(6)
            Layout.preferredHeight: QuickConnect.active ? quickConnectPanel.implicitHeight + Metrics.scaled(34) : 0
            visible: QuickConnect.active
            clip: true
            baseColor: Theme.accentPanel

            ColumnLayout {
                id: quickConnectPanel
                anchors.centerIn: parent
                width: Math.min(parent.width - Metrics.scaled(40), Metrics.scaled(420))
                spacing: Metrics.scaled(8)

                AppText {
                    Layout.alignment: Qt.AlignHCenter
                    text: QuickConnect.code
                    font.pixelSize: Metrics.scaled(32)
                    font.weight: Font.Bold
                    font.letterSpacing: Metrics.scaled(7)
                }

                SecondaryText {
                    Layout.fillWidth: true
                    text: QuickConnect.phase === "server" ? "Approved. Signing you in…" :
                                                            "Enter this code on a device that is already signed in to this server."
                    color: Theme.textSecondary
                    font.pixelSize: Metrics.metaSizePx + Metrics.scaled(1)
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
