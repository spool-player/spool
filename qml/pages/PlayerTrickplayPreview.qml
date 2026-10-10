import QtQuick
import Spool
import "../theme"
import "../primitives"

Item {
    id: root
    objectName: "playerTrickplayPreview"

    required property var overlay
    readonly property var previewPlayer: overlay.hasPlayer ? overlay.player : null
    readonly property bool localHovering: overlay.timelineHovering && overlay.controlsReason !== "remote"
    readonly property bool active: previewPlayer !== null && overlay.controlsVisible
                                   && previewPlayer.trickplayAvailable && (overlay.scrubbing || overlay.remoteScrubbing
                                                                           || localHovering || overlay.previewing)
    readonly property double previewSeconds: overlay.scrubbing || overlay.remoteScrubbing ? overlay.scrubSeconds :
                                                                                            localHovering
                                                                                            ? overlay.timelineHoverSeconds :
                                                                                              overlay.positionSeconds()
    property var trickplayData: ({})
    property bool selectionPending: active
    readonly property bool ready: trickplayData && trickplayData.available === true
    readonly property real previewScale: typeof Settings !== "undefined" ? Math.max(25, Math.min(200, Number(
                                                                                                     Settings.values["playback/trickplayPreviewScalePercent"])
                                                                                                 || 100)) / 100 : 1
    readonly property real aspect: ready && trickplayData.height > 0 ? trickplayData.width / trickplayData.height : 16
                                                                       / 9
    readonly property real displayWidth: ready ? Math.max(0, Math.min(Metrics.scaled(320) * previewScale, width - dp(104),
                                                                      Math.max(0, overlay.height - dp(252)) * aspect)) :
                                                 0
    readonly property real displayHeight: displayWidth / aspect
    readonly property real previewRatio: overlay.hasPlayer && overlay.player.durationSeconds > 0 ? Math.max(0, Math.min(
                                                                                                                1, previewSeconds
                                                                                                                / overlay.player.durationSeconds)) :
                                                                                                   0

    onActiveChanged: {
        selectionPending = active
        if (!active)
            trickplayData = ({})
    }
    onPreviewSecondsChanged: if (active)
                                 selectionPending = true
    onPreviewPlayerChanged: {
        trickplayData = ({})
        selectionPending = active
    }

    Connections {
        target: root.previewPlayer
        ignoreUnknownSignals: true
        function onTrickplayChanged() {
            if (!root.active)
                root.trickplayData = ({})
            root.selectionPending = root.active
        }
    }

    // Keep the scheduler outside the image-ready container so a pending
    // asynchronous image cannot prevent the first selection from running.
    FrameAnimation {
        running: root.active && root.selectionPending
        onTriggered: {
            root.selectionPending = false
            root.trickplayData = root.previewPlayer.trickplayForSeconds(root.previewSeconds)
        }
    }

    function dp(value) {
        return overlay.dp(value)
    }

    function sourceCrop() {
        return ready ? Qt.rect(-trickplayData.offsetX, -trickplayData.offsetY, trickplayData.width,
                               trickplayData.height) : Qt.rect(0, 0, 0, 0)
    }

    anchors.left: parent.left
    anchors.right: parent.right
    anchors.bottom: parent.bottom
    anchors.bottomMargin: dp(200)
    height: displayHeight + dp(32)
    visible: active
    z: 22

    Item {
        visible: root.ready && previewImage.ready
        readonly property real imageWidth: root.displayWidth
        readonly property real imageHeight: root.displayHeight
        x: Math.max(root.dp(52), Math.min(parent.width - imageWidth - root.dp(52), root.previewRatio * parent.width
                                          - imageWidth / 2))
        width: imageWidth
        height: imageHeight + root.dp(32)

        Rectangle {
            id: frame
            width: parent.imageWidth
            height: parent.imageHeight
            color: "transparent"
            border.color: Theme.borderStrong
            border.width: 1
            radius: Theme.radiusLarge
            clip: true

            TrickplayPreviewItem {
                id: previewImage
                objectName: "playerTrickplayTexture"
                anchors.fill: parent
                source: root.ready ? root.trickplayData.url : ""
                crop: root.sourceCrop()
            }
        }

        AppText {
            anchors.horizontalCenter: frame.horizontalCenter
            anchors.top: frame.bottom
            anchors.topMargin: root.dp(4)
            text: root.overlay.formatClock(root.previewSeconds)
            color: Theme.textPrimary
            // The same size as the clock under the title: this is the same
            // reading, just closer to the frame it belongs to.
            font.pixelSize: root.dp(22)
            font.weight: Font.Medium
        }
    }
}
