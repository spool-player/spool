import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

FocusScope {
    id: root
    required property var trustController
    required property var inputKeys
    property int actionIndex: 0

    anchors.fill: parent
    focus: visible

    function routeKey(key, phase, repeat) {
        if (root.inputKeys.isBack(key, false, false)) {
            // Back claims both phases; only the release dismisses.
            if (phase === "release")
                root.trustController.cancel()
            return true
        }
        if (!root.inputKeys.isDirection(key))
            return true
        if (phase === "press" && root.inputKeys.isHorizontal(key)) {
            const buttons = [cancelButton, onceButton, rememberButton]
            actionIndex = Math.max(0, Math.min(buttons.length - 1, actionIndex + (key === Qt.Key_Left ? -1 : 1)))
            root.inputKeys.focus(buttons[actionIndex])
        }
        return true
    }

    function activate() {
        if (actionIndex === 1)
            root.trustController.trustOnce()
        else if (actionIndex === 2)
            root.trustController.remember()
        else
            root.trustController.cancel()
    }

    function back() {
        root.trustController.cancel()
        return true
    }

    onVisibleChanged: {
        if (visible) {
            actionIndex = 0
            Qt.callLater(function () {
                root.inputKeys.focus(cancelButton)
            })
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#B8000000"
    }

    Surface {
        anchors.centerIn: parent
        width: Math.min(parent.width - Metrics.scaled(72), Metrics.scaled(760))
        height: Math.min(parent.height - Metrics.scaled(72), content.implicitHeight + Metrics.scaled(48))
        elevated: true
        baseColor: Theme.floatingPanel

        ColumnLayout {
            id: content
            anchors.fill: parent
            anchors.margins: Metrics.scaled(24)
            spacing: Metrics.scaled(14)

            AppText {
                Layout.fillWidth: true
                text: "Certificate trust required"
                font.pixelSize: Metrics.titleSizePx
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
            }

            AppText {
                Layout.fillWidth: true
                text: "The secure connection could not be verified. Compare this SHA-256 fingerprint with the server administrator before continuing."
                color: Theme.textSecondary
                font.pixelSize: Metrics.bodySizePx
                wrapMode: Text.Wrap
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Metrics.scaled(16)
                rowSpacing: Metrics.scaled(8)

                AppText {
                    text: "Connection"
                    color: Theme.textSecondary
                }
                SecondaryText {
                    Layout.fillWidth: true
                    text: root.trustController.pendingSource + " — " + root.trustController.pendingAuthority
                    wrapMode: Text.WrapAnywhere
                }
                AppText {
                    text: "Issuer"
                    color: Theme.textSecondary
                }
                SecondaryText {
                    Layout.fillWidth: true
                    text: root.trustController.pendingIssuer || "Not provided"
                    wrapMode: Text.WrapAnywhere
                }
                AppText {
                    text: "Fingerprint"
                    color: Theme.textSecondary
                }
                SecondaryText {
                    Layout.fillWidth: true
                    text: root.trustController.pendingFingerprint
                    wrapMode: Text.WrapAnywhere
                }
                AppText {
                    text: "TLS error"
                    color: Theme.textSecondary
                }
                AppText {
                    Layout.fillWidth: true
                    text: root.trustController.pendingErrors || "Certificate verification failed"
                    color: Theme.errorText
                    wrapMode: Text.Wrap
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Metrics.scaled(8)
                spacing: Metrics.scaled(10)

                ActionButton {
                    id: cancelButton
                    Layout.fillWidth: true
                    text: "Cancel"
                    onActiveFocusChanged: if (activeFocus)
                                              root.actionIndex = 0
                    onClicked: root.trustController.cancel()
                }
                ActionButton {
                    id: onceButton
                    Layout.fillWidth: true
                    text: "Trust Once"
                    onActiveFocusChanged: if (activeFocus)
                                              root.actionIndex = 1
                    onClicked: root.trustController.trustOnce()
                }
                ActionButton {
                    id: rememberButton
                    Layout.fillWidth: true
                    text: "Remember"
                    onActiveFocusChanged: if (activeFocus)
                                              root.actionIndex = 2
                    kind: "primary"
                    onClicked: root.trustController.remember()
                }
            }
        }
    }
}
