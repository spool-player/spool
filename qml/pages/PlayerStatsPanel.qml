pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import Spool
import "../theme"
import "../primitives"

// mpv paints its stats page into the video output, and audio playback asks for
// no video surface — so for music the page would never be drawn at all. This is
// that page, in mpv's own readings and mpv's own formatting, painted by the UI.
Item {
    id: root

    readonly property bool live: Player.sessionActive && Player.debugOsdVisible
    property string page: ""

    function refresh() {
        page = live ? Player.mpvStatsPage() : ""
    }

    onLiveChanged: refresh()

    Timer {
        interval: 1000
        repeat: true
        running: root.live && root.visible
        onTriggered: root.refresh()
    }

    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: Metrics.scaled(18)
        width: Metrics.scaled(460)
        height: statsColumn.implicitHeight + Metrics.scaled(24)
        visible: root.page.length > 0
        color: "#E6161616"
        border.width: 1
        border.color: Theme.borderStrong
        radius: Theme.radiusMedium

        ColumnLayout {
            id: statsColumn
            anchors.fill: parent
            anchors.margins: Metrics.scaled(12)
            spacing: Metrics.scaled(4)

            SecondaryText {
                text: "mpv"
                color: Theme.textPrimary
                font.weight: Font.DemiBold
            }

            SecondaryText {
                Layout.fillWidth: true
                Layout.maximumWidth: Metrics.scaled(436)
                text: root.page
                wrapMode: Text.Wrap
                lineHeight: 1.15
            }
        }
    }
}
