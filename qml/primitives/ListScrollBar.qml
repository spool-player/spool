import QtQuick
import "../theme"

Item {
    id: root

    required property Flickable flickable
    property bool interactive: true
    property real minimumSize: 0.04
    readonly property int visualWidth: Math.max(Metrics.scaled(10), 10)
    readonly property bool hovered: scrubber.containsMouse || scrubber.pressed
    readonly property bool pressed: scrubber.pressed
    readonly property real handleCenterY: handle.y + handle.height / 2
    signal scrolled

    // Softened corners rather than a full pill; the track and handle should
    // read as a slim bar, not a lozenge.
    function cornerRadius(width) {
        return Math.max(1, Math.round(width * 0.22))
    }

    function setContentY(value) {
        const before = root.flickable.contentY
        root.flickable.contentY = value
        if (root.flickable.contentY !== before)
            root.scrolled()
    }

    readonly property real scrollRange: Math.max(0, flickable.contentHeight - flickable.height)
    readonly property real availableTrack: Math.max(0, height - handle.height)
    readonly property real visibleFraction: flickable.contentHeight > 0 ? Math.min(1, flickable.height
                                                                                   / flickable.contentHeight) : 1

    visible: scrollRange > 0
    implicitWidth: Math.ceil(visualWidth * 3)

    Rectangle {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: root.visualWidth
        radius: root.cornerRadius(width)
        color: Theme.accentPanel
        opacity: root.hovered ? 0.9 : 0.55

        Behavior on opacity {
            NumberAnimation {
                duration: Theme.reducedMotion ? 0 : 90
            }
        }
    }

    Rectangle {
        id: handle

        width: Math.max(Metrics.scaled(6), 6)
        height: Math.max(root.height * root.minimumSize, root.height * root.visibleFraction)
        x: root.width - (root.visualWidth + width) / 2
        y: root.scrollRange > 0 ? root.availableTrack * (root.flickable.contentY - root.flickable.originY)
                                  / root.scrollRange : 0
        radius: root.cornerRadius(width)
        color: Theme.accent
    }

    // One grab for the whole strip: pressing the track jumps the handle under
    // the cursor and continues as a drag, while pressing the handle keeps its
    // grab offset so fine dragging never lurches. An exclusive grab on press is
    // what makes this work at all - a TapHandler only takes a passive grab, so
    // any MouseArea layered over the bar cancels it before the release lands.
    MouseArea {
        id: scrubber

        property real grabOffset: 0

        function scrubTo(y) {
            if (root.availableTrack <= 0)
                return
            const targetY = Math.max(0, Math.min(root.availableTrack, y - grabOffset))
            root.setContentY(root.flickable.originY + targetY * root.scrollRange / root.availableTrack)
        }

        anchors.fill: parent
        enabled: root.interactive
        hoverEnabled: true
        preventStealing: true
        acceptedButtons: Qt.LeftButton

        onPressed: mouse => {
            const onHandle = mouse.y >= handle.y && mouse.y <= handle.y + handle.height
            grabOffset = onHandle ? mouse.y - handle.y : handle.height / 2
            if (!onHandle)
                scrubTo(mouse.y)
        }
        onPositionChanged: mouse => {
            if (pressed)
                scrubTo(mouse.y)
        }
    }
}
