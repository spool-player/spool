import QtQuick

Item {
    id: root

    required property GridView view
    signal activated(int index, int button)
    signal contextRequested(int index)
    signal contextGestureEnded

    // Keep hit testing on the stationary viewport, not its moving delegates.
    parent: view
    width: view.width
    height: view.height
    z: 3

    TapHandler {
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        gesturePolicy: TapHandler.DragThreshold
        longPressThreshold: 0.52
        property int pressedIndex: -1
        property bool pressedWhileMoving: false
        property bool held: false

        onPressedChanged: {
            if (pressed) {
                held = false
                pressedWhileMoving = root.view.moving
                pressedIndex = root.view.indexAt(point.position.x + root.view.contentX, point.position.y
                                                 + root.view.contentY)
            } else if (held) {
                root.contextGestureEnded()
            }
        }
        onTapped: (eventPoint, button) => {
            if (!held && !pressedWhileMoving && !root.view.moving && pressedIndex >= 0)
                root.activated(pressedIndex, button)
        }
        onLongPressed: {
            if (pressedWhileMoving || root.view.moving || pressedIndex < 0)
                return
            held = true
            root.contextRequested(pressedIndex)
        }
    }
}
