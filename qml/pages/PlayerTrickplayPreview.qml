import QtQuick
import "../theme"
import "../primitives"

Item {
    id: root

    required property var overlay
    readonly property bool localHovering: overlay.timelineHovering && overlay.controlsReason !== "remote"
    readonly property bool active: overlay.hasPlayer && overlay.controlsVisible && overlay.player.trickplayAvailable && (
                                       overlay.scrubbing || overlay.remoteScrubbing || localHovering
                                       || overlay.previewing)
    readonly property double previewSeconds: overlay.scrubbing || overlay.remoteScrubbing ? overlay.scrubSeconds :
                                                                                            localHovering
                                                                                            ? overlay.timelineHoverSeconds :
                                                                                              overlay.positionSeconds()
    readonly property var trickplayData: active ? overlay.player.trickplayForSeconds(previewSeconds) : ({})
    readonly property bool ready: trickplayData && trickplayData.available === true
    readonly property real scaleFactor: overlay.uiScale * 1.4
    readonly property real previewRatio: overlay.hasPlayer && overlay.player.durationSeconds > 0 ? Math.max(0, Math.min(
                                                                                                                1, previewSeconds
                                                                                                                / overlay.player.durationSeconds)) :
                                                                                                   0

    function dp(value) {
        return overlay.dp(value)
    }

    function artworkSource(url) {
        if (!url)
            return ""
        return url.indexOf("http://") === 0 || url.indexOf("https://") === 0 ? "image://artwork/" + encodeURIComponent(
                                                                                   url) : url
    }

    anchors.left: parent.left
    anchors.right: parent.right
    anchors.bottom: parent.bottom
    anchors.bottomMargin: dp(200)
    height: ready ? Math.round((trickplayData.height || 0) * scaleFactor) + dp(32) : 0
    visible: ready
    z: 22

    Item {
        readonly property real imageWidth: root.ready ? root.trickplayData.width * root.scaleFactor : 0
        readonly property real imageHeight: root.ready ? root.trickplayData.height * root.scaleFactor : 0
        x: Math.max(root.dp(52), Math.min(parent.width - imageWidth - root.dp(52), root.previewRatio * parent.width
                                          - imageWidth / 2))
        width: imageWidth
        height: imageHeight + root.dp(32)

        Rectangle {
            id: frame
            width: parent.imageWidth
            height: parent.imageHeight
            color: "black"
            border.color: Theme.borderStrong
            border.width: 1
            radius: Theme.radiusLarge
            clip: true

            Image {
                source: root.ready ? root.artworkSource(root.trickplayData.url) : ""
                x: root.ready ? root.trickplayData.offsetX * root.scaleFactor : 0
                y: root.ready ? root.trickplayData.offsetY * root.scaleFactor : 0
                width: root.ready ? root.trickplayData.sheetWidth * root.scaleFactor : 0
                height: root.ready ? root.trickplayData.sheetHeight * root.scaleFactor : 0
                fillMode: Image.Stretch
                cache: true
                asynchronous: true
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
