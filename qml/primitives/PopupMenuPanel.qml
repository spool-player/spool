import QtQuick
import "../theme"

Surface {
    id: root
    default property alias content: body.data
    property bool open: false
    property real openHeight: 0

    height: open ? openHeight : 0
    visible: open
    z: 21
    baseColor: Theme.bgRaised
    elevated: true
    clip: true

    // Inert rows and panel padding belong to popup, not its dismiss backdrop.
    // Block pointer handlers behind panel while keeping content handlers above
    // this shield free to give drags to their Flickables.
    MouseArea {
        anchors.fill: parent

        TapHandler {
            gesturePolicy: TapHandler.ReleaseWithinBounds
        }
    }

    Item {
        id: body
        anchors.fill: parent
    }
}
