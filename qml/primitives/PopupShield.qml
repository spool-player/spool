import QtQuick

// Pointer shield for a popup surface. A bare MouseArea stops the MouseAreas
// beneath it, but TapHandlers only take a passive grab and see every press
// regardless; the exclusive-grab TapHandler here is what stops them. Declare
// it before the popup's content so content handlers stay above it and remain
// free to hand a drag to their Flickable.
MouseArea {
    anchors.fill: parent

    TapHandler {
        gesturePolicy: TapHandler.ReleaseWithinBounds
    }
}
