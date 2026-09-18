pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

RowLayout {
    id: root

    required property var overlay
    Layout.fillWidth: true
    spacing: overlay.dp(8)
    function actionTarget(action) {
        const index = overlay.utilityActions.indexOf(action)
        return index >= 0 ? utilityRepeater.itemAt(index) : null
    }

    component ActionTarget: Rectangle {
        required property string action
        readonly property int globalIndex: root.overlay.actions.indexOf(action)
        readonly property bool focused: root.overlay.isControlsActive() && root.overlay.focusZone === "actions"
                                        && root.overlay.actionIndex === globalIndex
        readonly property bool selected: action === "syncplay" && root.overlay.syncPlayMenuOpen
        readonly property bool emphasized: focused || selected || (hover.hovered && Metrics.pointerActive)
        readonly property string tooltip: Settings.playerControlTooltipsEnabled ? root.overlay.actionTooltip(action) :
                                                                                  ""
        readonly property bool waitingForSyncPlay: action === "pause" && root.overlay.syncPlayWaiting
        readonly property string badge: action === "prevQueue" || action === "nextQueue" ? "EP" : action
                                                                                           === "prevChapter" || action
                                                                                           === "nextChapter" ? "CH" : ""
        Layout.preferredWidth: root.overlay.actionTargetSize
        Layout.preferredHeight: root.overlay.actionTargetSize
        radius: width / 2
        color: emphasized ? Qt.alpha(Theme.accent, 0.2) : "transparent"
        border.width: emphasized ? Theme.focusBorderWidth : 0
        border.color: Theme.accent

        MaterialIcon {
            anchors.centerIn: parent
            name: parent.action.length > 0 ? root.overlay.actionIcon(parent.action) : ""
            iconColor: parent.emphasized ? Theme.textPrimary : Theme.textSecondary
            iconSize: root.overlay.dp(parent.action === "debug" ? 32 : 38)
        }

        MaterialIcon {
            anchors.centerIn: parent
            anchors.horizontalCenterOffset: root.overlay.dp(9)
            anchors.verticalCenterOffset: root.overlay.dp(7)
            visible: parent.waitingForSyncPlay
            name: "play_arrow"
            iconColor: parent.focused ? Theme.textPrimary : Theme.textSecondary
            iconSize: root.overlay.dp(20)
        }
        AppText {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.rightMargin: root.overlay.dp(5)
            anchors.bottomMargin: root.overlay.dp(5)
            visible: parent.badge.length > 0
            text: parent.badge
            color: parent.emphasized ? Theme.textPrimary : Theme.textSecondary
            font.pixelSize: root.overlay.dp(10)
            font.weight: Font.Bold
        }

        HoverHandler {
            id: hover
        }

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.top
            anchors.bottomMargin: root.overlay.dp(8)
            width: tooltipText.implicitWidth + root.overlay.dp(20)
            height: root.overlay.dp(34)
            radius: root.overlay.dp(6)
            color: "#E6222222"
            border.width: 1
            border.color: Theme.borderStrong
            visible: parent.tooltip.length > 0 && (parent.focused || hover.hovered)
            z: 10

            AppText {
                id: tooltipText
                anchors.centerIn: parent
                text: parent.parent.tooltip
                color: Theme.textPrimary
                font.pixelSize: root.overlay.dp(15)
                font.weight: Font.Medium
            }
        }

        TapHandler {
            onTapped: {
                root.overlay.controlsVisible = true
                root.overlay.focusZone = "actions"
                root.overlay.actionIndex = parent.globalIndex
                root.overlay.activateAction()
            }
        }
    }

    // Video spreads the controls across the foot of the picture. Audio has no
    // picture to frame, so the same controls gather into one centred cluster
    // under the record.
    Item {
        Layout.fillWidth: root.overlay.audioOnly
    }

    Repeater {
        model: root.overlay.transportActions
        delegate: ActionTarget {
            required property string modelData
            action: modelData
        }
    }

    Item {
        Layout.fillWidth: !root.overlay.audioOnly
        Layout.preferredWidth: root.overlay.audioOnly ? root.overlay.dp(40) : 0
    }

    Repeater {
        id: utilityRepeater
        model: root.overlay.utilityActions
        delegate: ActionTarget {
            required property string modelData
            action: modelData
        }
    }

    RowLayout {
        id: volumeControls

        readonly property bool showSlider: Settings.values["playback/showVolumeSlider"] !== false
        property real lastAudibleVolume: 100

        visible: root.overlay.desktopControlsAvailable
        Layout.minimumWidth: visible ? root.overlay.dp(showSlider ? 254 : 28) : 0
        Layout.preferredWidth: visible ? root.overlay.dp(showSlider ? 254 : 28) : 0
        Layout.maximumWidth: visible ? root.overlay.dp(showSlider ? 254 : 28) : 0
        spacing: root.overlay.dp(10)

        MaterialIcon {
            name: root.overlay.hasPlayer && root.overlay.player.volume === 0 ? "volume_off" : "volume_up"
            iconColor: muteHover.hovered ? Theme.textPrimary : Theme.textSecondary
            iconSize: root.overlay.dp(38)

            HoverHandler {
                id: muteHover
            }

            TapHandler {
                onTapped: {
                    if (!root.overlay.hasPlayer)
                        return
                    const volume = Number(root.overlay.player.volume)
                    if (volume > 0) {
                        volumeControls.lastAudibleVolume = volume
                        root.overlay.player.setVolume(0)
                    } else {
                        root.overlay.player.setVolume(Math.max(1, volumeControls.lastAudibleVolume))
                    }
                }
            }
        }

        InlineSlider {
            visible: parent.showSlider
            Layout.minimumWidth: visible ? root.overlay.dp(144) : 0
            Layout.preferredWidth: visible ? root.overlay.dp(144) : 0
            Layout.maximumWidth: visible ? root.overlay.dp(144) : 0
            from: 0
            to: 100
            stepSize: 1
            value: root.overlay.hasPlayer ? root.overlay.player.volume : 100
            barHeight: root.overlay.dp(8)
            handleSize: root.overlay.dp(20)
            interactionMargin: handleSize
            onMoved: newValue => {
                if (root.overlay.hasPlayer)
                    root.overlay.player.setVolume(Math.round(newValue))
            }
        }

        AppText {
            visible: parent.showSlider
            text: root.overlay.hasPlayer ? Math.round(root.overlay.player.volume) + "%" : "100%"
            color: Theme.textSecondary
            font.pixelSize: root.overlay.dp(18)
            font.weight: Font.DemiBold
        }
    }
}
