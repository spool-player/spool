import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

FocusScope {
    id: root
    required property var trustController
    required property var inputKeys

    signal dismissed
    anchors.fill: parent
    focus: true

    function routeKey(key, phase, repeat) {
        if (root.inputKeys.isBack(key, false, false)) {
            if (phase === "press" && !repeat)
                dismissed()
            return phase === "press"
        }
        if (!root.inputKeys.isDirection(key))
            return true
        if (phase !== "press")
            return true
        if (root.inputKeys.isHorizontal(key)) {
            root.inputKeys.focus(closeButton.activeFocus ? forgetButton : closeButton)
            return true
        }
        if (key === Qt.Key_Up || key === Qt.Key_Down) {
            const delta = key === Qt.Key_Up ? -1 : 1
            if (certificates.count > 0)
                certificates.currentIndex = Math.max(0, Math.min(certificates.count - 1, certificates.currentIndex
                                                                 + delta))
            root.inputKeys.focus(certificates)
        }
        return true
    }

    function activate() {
        if (forgetButton.activeFocus && certificates.currentItem)
            root.trustController.removeRemembered(String(certificates.currentItem.certificateKey))
        else if (certificates.activeFocus)
            root.inputKeys.focus(forgetButton)
        else
            dismissed()
    }

    function back() {
        dismissed()
        return true
    }

    Component.onCompleted: Qt.callLater(function () {
        root.inputKeys.focus(closeButton)
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
        width: Math.min(parent.width - Metrics.scaled(72), Metrics.scaled(760))
        height: Math.min(parent.height - Metrics.scaled(72), Metrics.scaled(600))
        elevated: true
        baseColor: Theme.floatingPanel

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Metrics.scaled(24)
            spacing: Metrics.scaled(14)

            AppText {
                Layout.fillWidth: true
                text: "Remembered Certificates"
                font.pixelSize: Metrics.titleSizePx
                font.weight: Font.DemiBold
            }

            AppText {
                Layout.fillWidth: true
                text: "These fingerprints are trusted only for the exact server and port shown."
                color: Theme.textSecondary
                wrapMode: Text.Wrap
            }

            ListView {
                id: certificates
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: Metrics.scaled(8)
                model: root.trustController.rememberedCertificates
                currentIndex: count > 0 ? Math.max(0, Math.min(currentIndex, count - 1)) : -1
                boundsBehavior: Flickable.StopAtBounds

                delegate: Surface {
                    id: certificateRow
                    required property var modelData
                    readonly property string certificateKey: String(modelData.key || "")
                    width: ListView.view.width
                    height: details.implicitHeight + Metrics.scaled(24)
                    elevated: ListView.isCurrentItem
                    baseColor: ListView.isCurrentItem ? Theme.focusedFill : Theme.bgRaised

                    TapHandler {
                        onTapped: {
                            certificates.currentIndex = index
                            root.inputKeys.focus(forgetButton)
                        }
                    }

                    ColumnLayout {
                        id: details
                        anchors.fill: parent
                        anchors.margins: Metrics.scaled(12)
                        spacing: Metrics.scaled(4)
                        AppText {
                            Layout.fillWidth: true
                            text: String(modelData.authority || "")
                            font.weight: Font.DemiBold
                        }
                        SecondaryText {
                            Layout.fillWidth: true
                            text: String(modelData.fingerprint || "")
                            wrapMode: Text.WrapAnywhere
                        }
                        AppText {
                            Layout.fillWidth: true
                            text: "Issuer: " + String(modelData.issuer || "Not provided")
                            color: Theme.textSecondary
                            wrapMode: Text.Wrap
                        }
                    }
                }

                AppText {
                    anchors.centerIn: parent
                    visible: certificates.count === 0
                    text: "No certificates are remembered."
                    color: Theme.textSecondary
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Metrics.scaled(10)
                Item {
                    Layout.fillWidth: true
                }
                ActionButton {
                    id: closeButton
                    text: "Close"
                    onClicked: root.dismissed()
                }
                ActionButton {
                    id: forgetButton
                    text: "Forget Selected"
                    kind: "danger"
                    enabled: certificates.count > 0
                    onClicked: {
                        if (certificates.currentItem)
                            root.trustController.removeRemembered(String(certificates.currentItem.certificateKey))
                    }
                }
            }
        }
    }
}
