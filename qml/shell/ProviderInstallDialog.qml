pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

OverlayDialog {
    id: root
    required property var transfers
    property bool suspendLocalTransfers: false
    readonly property var visibleTransfers: suspendLocalTransfers ? transfers.filter(transfer =>
    !transfer.operationToken) : transfers
    visible: visibleTransfers.length > 0
    focus: visible
    preferredWidth: 620

    function routeKey(key, phase, repeat) {
        return visible
    }
    function activate() {
    }
    function back() {
        return visible
    }

    AppText {
        Layout.fillWidth: true
        text: "Installing providers"
        font.pixelSize: Metrics.titleSizePx
        font.weight: Font.DemiBold
    }
    Repeater {
        model: root.visibleTransfers
        delegate: ColumnLayout {
            id: transfer
            required property var modelData
            Layout.fillWidth: true
            spacing: Metrics.scaled(10)
            readonly property bool downloading: modelData.state === "downloading"
            readonly property bool determinate: downloading && modelData.total > 0
            readonly property real fraction: determinate ? Math.min(1, Math.max(0, modelData.received
                                                                                / modelData.total)) : 0

            AppText {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: (transfer.downloading ? "Downloading " : "Verifying and installing ") + transfer.modelData.name
                      + "…"
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
            RowLayout {
                Layout.fillWidth: true
                BusySpinner {
                    visible: !transfer.determinate
                    Layout.preferredWidth: Metrics.scaled(28)
                    Layout.preferredHeight: Metrics.scaled(28)
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Metrics.scaled(10)
                    radius: height / 2
                    color: Theme.bgRaised
                    clip: true
                    visible: transfer.determinate
                    Accessible.role: Accessible.ProgressBar
                    Accessible.name: transfer.modelData.name
                    Accessible.description: Math.round(transfer.fraction * 100) + "%"
                    Rectangle {
                        width: parent.width * transfer.fraction
                        height: parent.height
                        radius: parent.radius
                        color: Theme.accent
                    }
                }
                AppText {
                    visible: transfer.determinate
                    text: Math.round(transfer.fraction * 100) + "%"
                }
            }
        }
    }
    AppText {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: "This closes automatically when installation finishes."
        color: Theme.textSecondary
    }
}
