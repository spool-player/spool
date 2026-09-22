pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import Spool
import "../theme"
import "../primitives"

OverlayDialog {
    id: root

    required property var overlay
    visible: overlay.audioSyncVisible
    preferredWidth: 760
    z: 45
    onDismissed: overlay.closeAudioSync()

    AppText {
        Layout.fillWidth: true
        text: root.overlay.syncTarget === "subtitle" ? "Subtitle sync" : "Audio sync"
        color: Theme.textPrimary
        font.pixelSize: Metrics.titleSizePx
        font.weight: Font.DemiBold
    }

    RowLayout {
        Layout.fillWidth: true
        visible: root.overlay.syncTarget !== "subtitle"
        Layout.preferredHeight: visible ? implicitHeight : 0
        spacing: 10

        ActionButton {
            Layout.fillWidth: true
            text: "This file"
            kind: root.overlay.syncTarget === "audioFile" ? "primary" : "secondary"
            onClicked: {
                root.overlay.syncTarget = "audioFile"
                root.overlay.audioSyncRow = "target"
            }
        }
        ActionButton {
            Layout.fillWidth: true
            text: Settings.audioDelayTargetLabel
            kind: root.overlay.syncTarget === "audioOutput" ? "primary" : "secondary"
            onClicked: {
                root.overlay.syncTarget = "audioOutput"
                root.overlay.audioSyncRow = "target"
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 18

        ActionButton {
            text: "−"
            kind: root.overlay.audioSyncRow === "delay" ? "primary" : "secondary"
            onClicked: {
                root.overlay.audioSyncRow = "delay"
                root.overlay.adjustAudioDelay(-1)
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4
            AppText {
                Layout.fillWidth: true
                text: root.overlay.formatAudioDelay(root.overlay.currentSyncDelayMs)
                color: Theme.textPrimary
                font.pixelSize: Math.round(Metrics.titleSizePx * 1.8)
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignHCenter
            }
            AppText {
                Layout.fillWidth: true
                text: root.overlay.syncTarget === "subtitle" ? "Negative values show subtitles earlier" :
                                                               "Negative values advance audio; positive values delay it"
                color: Theme.textMuted
                font.pixelSize: Metrics.metaSizePx
                horizontalAlignment: Text.AlignHCenter
            }
            AppText {
                Layout.fillWidth: true
                visible: root.overlay.syncTarget !== "subtitle"
                text: "Effective A/V correction: " + root.overlay.formatAudioDelay(
                          root.overlay.player.effectiveAudioDelayMs - Settings.displayLatencyMs)
                color: Theme.textMuted
                font.pixelSize: Metrics.metaSizePx
                horizontalAlignment: Text.AlignHCenter
            }
        }

        ActionButton {
            text: "+"
            kind: root.overlay.audioSyncRow === "delay" ? "primary" : "secondary"
            onClicked: {
                root.overlay.audioSyncRow = "delay"
                root.overlay.adjustAudioDelay(1)
            }
        }
    }

    AppText {
        Layout.fillWidth: true
        text: "Step size"
        color: root.overlay.audioSyncRow === "step" ? Theme.textPrimary : Theme.textSecondary
        font.pixelSize: Metrics.bodySizePx
        font.weight: Font.DemiBold
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 10

        Repeater {
            model: root.overlay.audioSyncSteps.length
            delegate: ActionButton {
                required property int index
                Layout.fillWidth: true
                text: root.overlay.audioSyncSteps[index] + " ms"
                kind: root.overlay.audioSyncStepIndex === index ? "primary" : "secondary"
                onClicked: {
                    root.overlay.audioSyncRow = "step"
                    root.overlay.audioSyncStepIndex = index
                }
            }
        }
    }
}
