import QtQuick
import "../theme"

Item {
    id: root

    property real from: 0
    property real to: 1
    property real value: from
    property real stepSize: 0
    property real barHeight: 6
    property real handleSize: 16
    property real interactionMargin: 0
    property real minimumInteractionHeight: 48
    // Set when the owning row has focus, so the track reads as the thing the
    // remote is about to change.
    property bool highlighted: false
    // Paint the filled part in the accent colour, the way the player's own seek
    // bar does. Off by default so the settings sliders keep their quieter look.
    property bool accented: false
    property bool dragging: false

    // The reading the handle draws at. A drag keeps its own value rather than
    // assigning `value`, because assigning a property from JavaScript destroys
    // whatever binding its owner set on it — after one drag the slider would
    // stop tracking the thing it is supposed to display.
    property real dragValue: from
    readonly property real effectiveValue: dragging ? dragValue : value

    signal moved(real newValue)
    signal committed(real newValue)

    readonly property real range: Math.max(0, to - from)
    readonly property real position: range > 0 ? Math.max(0, Math.min(1, (effectiveValue - from) / range)) : 0

    implicitHeight: Math.max(handleSize, barHeight)

    function valueAt(pointerX) {
        const fraction = Math.max(0, Math.min(1, pointerX / Math.max(1, width)))
        let next = from + fraction * range
        if (stepSize > 0)
            next = Math.round((next - from) / stepSize) * stepSize + from
        return Math.max(from, Math.min(to, next))
    }

    function updateValue(pointerX) {
        const next = valueAt(pointerX);
        // A touch stream reports far more points than the bar has pixels, and
        // every repeat would re-run the owner's handler for a value it already
        // holds. Only a real change is worth reporting.
        if (dragging && dragValue === next)
            return
        dragValue = next
        moved(next)
    }

    Rectangle {
        id: track
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: root.barHeight
        radius: height / 2
        color: Theme.borderStrong

        Rectangle {
            width: parent.width * root.position
            height: parent.height
            radius: parent.radius
            color: root.accented || root.highlighted ? Theme.accent : Theme.textSecondary
        }
    }

    Rectangle {
        x: root.position * (root.width - width)
        anchors.verticalCenter: parent.verticalCenter
        width: root.handleSize
        height: width
        radius: width / 2
        color: Theme.textPrimary
        border.width: root.highlighted || (root.accented && root.dragging) ? Theme.focusBorderWidth : 1
        border.color: root.highlighted || root.accented ? Theme.accent : Theme.bg
    }

    MouseArea {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        width: root.width + root.interactionMargin * 2
        height: Math.max(root.height + root.interactionMargin * 2, root.minimumInteractionHeight)
        acceptedButtons: Qt.LeftButton
        preventStealing: true

        function pointerX(mouse) {
            return mouse.x - root.interactionMargin
        }

        onPressed: mouse => {
            // Start from where the bar is actually showing, so the first move
            // is measured against the current reading rather than the value
            // left behind by the previous drag.
            root.dragValue = root.value
            root.dragging = true
            root.updateValue(pointerX(mouse))
        }
        onPositionChanged: mouse => {
            if (pressed)
                root.updateValue(pointerX(mouse))
        }
        onReleased: {
            if (!root.dragging)
                return
            root.dragging = false
            root.committed(root.dragValue)
        }
        onCanceled: root.dragging = false
    }
}
