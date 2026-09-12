pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

Item {
    id: root

    required property var overlay
    readonly property double ratio: overlay.hasPlayer && overlay.player.durationSeconds > 0 ? Math.max(0, Math.min(1,
                                                                                                                   overlay.positionSeconds(
                                                                                                                       ) / overlay.player.durationSeconds)) :
                                                                                              0
    readonly property bool focused: overlay.isControlsActive() && overlay.focusZone === "timeline"
    readonly property bool hasActiveSegment: overlay.hasPlayer && overlay.player.activeSegmentType.length > 0
                                             && overlay.player.durationSeconds > 0

    function dp(value) {
        return overlay.dp(value)
    }

    function endsAtText() {
        // A track is over in minutes: the clock time it ends at is noise.
        if (overlay.audioOnly || !overlay.hasPlayer || overlay.player.durationSeconds <= 0)
            return ""
        const remainingSeconds = Math.max(0, overlay.player.durationSeconds - overlay.positionSeconds())
        const playbackSpeed = Math.max(0.01, Number(overlay.player.effectivePlaybackSpeed))
        const endTime = new Date(Date.now() + remainingSeconds / playbackSpeed * 1000)
        return qsTr("Ends at %1").arg(endTime.toLocaleTimeString(Qt.locale(), Locale.ShortFormat))
    }

    Layout.fillWidth: true
    Layout.preferredHeight: dp(76)

    AppText {
        anchors.left: parent.left
        anchors.top: parent.top
        text: root.overlay.formatClock(root.overlay.positionSeconds())
        color: root.focused ? Theme.textPrimary : Theme.textSecondary
        font.pixelSize: root.dp(22)
        font.weight: Font.Medium
    }
    AppText {
        anchors.right: parent.right
        anchors.top: parent.top
        text: {
            const duration = root.overlay.hasPlayer ? root.overlay.player.durationSeconds : 0
            const endsAt = root.endsAtText()
            return endsAt.length > 0 ? root.overlay.formatClock(duration) + "  ·  " + endsAt : root.overlay.formatClock(
                                           duration)
        }
        color: Theme.textSecondary
        font.pixelSize: root.dp(22)
        font.weight: Font.Medium
    }

    Rectangle {
        id: track
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.dp(18)
        height: root.focused ? root.dp(16) : root.dp(10)
        // Barely softened rather than a pill: a rounded cap on a progress bar
        // reads as part of the fill and blurs where playback actually sits.
        radius: Math.max(1, root.dp(2))
        color: Theme.borderStrong

        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: Math.max(parent.height, parent.width * root.ratio)
            radius: parent.radius
            color: root.overlay.hasPlayer && root.overlay.player.buffering ? Qt.lighter(Theme.accent, 1.2) :
                                                                             Theme.accent
        }

        Repeater {
            model: root.overlay.hasPlayer ? root.overlay.player.chapters : []
            delegate: Rectangle {
                required property var modelData
                readonly property double startRatio: root.overlay.hasPlayer && root.overlay.player.durationSeconds > 0
                                                     ? Math.max(0, Math.min(1, (modelData.start || 0)
                                                                            / root.overlay.player.durationSeconds)) : 0
                x: Math.max(0, Math.min(track.width - width, track.width * startRatio - width / 2))
                anchors.verticalCenter: track.verticalCenter
                width: Math.max(1, root.dp(2))
                height: track.height
                color: Theme.textPrimary
                opacity: 0.5
                visible: startRatio > 0.002 && startRatio < 0.998
            }
        }

        Rectangle {
            readonly property double endRatio: root.hasActiveSegment ? Math.max(0, Math.min(1,
                                                                                            root.overlay.player.activeSegmentEndSeconds
                                                                                            / root.overlay.player.durationSeconds)) :
                                                                       0
            x: Math.max(0, Math.min(track.width - width, track.width * endRatio - width / 2))
            anchors.verticalCenter: track.verticalCenter
            width: Math.max(2, root.dp(3))
            height: track.height + root.dp(10)
            radius: width / 2
            color: Theme.accent
            visible: root.hasActiveSegment
        }

        Rectangle {
            x: Math.max(0, Math.min(track.width - width, track.width * root.ratio - width / 2))
            anchors.verticalCenter: track.verticalCenter
            width: root.focused || root.overlay.scrubbing ? root.dp(30) : root.dp(18)
            height: width
            radius: width / 2
            color: Theme.textPrimary
            border.width: 3
            border.color: Theme.accent
        }
    }

    MouseArea {
        id: hoverArea

        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true

        function secondsForX(x) {
            return root.overlay.clampSeconds(x / Math.max(1, width) * (root.overlay.hasPlayer
                                                                       ? root.overlay.player.durationSeconds : 0))
        }

        function updateHover(x) {
            root.overlay.timelineHoverSeconds = secondsForX(x)
            root.overlay.timelineHovering = true
        }

        function updatePosition(mouse) {
            root.overlay.timelineHovering = false
            root.overlay.showControls("timeline")
            root.overlay.scrubbing = true
            root.overlay.scrubSeconds = secondsForX(mouse.x)
        }

        onEntered: updateHover(mouseX)
        onExited: root.overlay.timelineHovering = false
        onPressed: mouse => updatePosition(mouse)
        onPositionChanged: mouse => {
            if (pressed)
                updatePosition(mouse)
            else
                updateHover(mouse.x)
        }
        onReleased: mouse => {
            updatePosition(mouse)
            root.overlay.commitScrub()
            if (containsMouse)
                updateHover(mouse.x)
        }
        onCanceled: {
            root.overlay.scrubbing = false
            if (containsMouse)
                updateHover(mouseX)
        }
    }
}
