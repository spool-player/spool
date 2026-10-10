import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

Rectangle {
    id: root

    required property var overlay
    readonly property string segmentType: overlay.hasPlayer ? overlay.player.activeSegmentType : ""
    readonly property bool focused: overlay.isControlsActive() && overlay.focusZone === "skip"
    readonly property string label: {
        if (segmentType === "Intro")
            return "Skip intro"
        if (segmentType === "Outro")
            return "Skip outro"
        if (segmentType === "Recap")
            return "Skip recap"
        if (segmentType === "Preview")
            return "Skip preview"
        return segmentType.length > 0 ? "Skip " + segmentType : ""
    }

    anchors.right: parent.right
    anchors.bottom: parent.bottom
    anchors.rightMargin: overlay.dp(48)
    anchors.bottomMargin: overlay.dp(220)
    width: overlay.dp(220)
    height: overlay.dp(60)
    radius: Theme.radiusPanel
    color: focused || skipHover.hovered ? Qt.lighter(Theme.accentDim, 1.2) : Theme.accentDim
    border.width: focused ? Theme.focusBorderWidth : 1
    border.color: Theme.accent
    visible: overlay.hasActiveSegment && overlay.controlsVisible
    z: 30

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: root.overlay.dp(16)
        anchors.rightMargin: root.overlay.dp(16)
        spacing: root.overlay.dp(10)

        MaterialIcon {
            name: "skip_next"
            iconColor: Theme.textPrimary
            iconSize: root.overlay.dp(24)
        }
        AppText {
            Layout.fillWidth: true
            text: root.label
            color: Theme.textPrimary
            font.pixelSize: root.overlay.dp(18)
            font.weight: Font.DemiBold
        }
        AppText {
            text: root.overlay.smartTvPlatform ? (root.focused ? "OK" : "") : root.overlay.touchscreenControls ? "" :
                                                                                                                 "T"
            visible: text.length > 0
            color: Theme.textSecondary
            font.pixelSize: root.overlay.dp(13)
        }
    }

    HoverHandler {
        id: skipHover
    }

    TapHandler {
        onTapped: root.overlay.skipActiveSegment()
    }
}
