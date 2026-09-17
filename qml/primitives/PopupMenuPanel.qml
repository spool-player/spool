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

    // Inert rows and panel padding belong to the popup, not its dismiss
    // backdrop. Row handlers remain free to give a drag to their Flickable.
    PopupShield {}

    Item {
        id: body
        anchors.fill: parent
    }
}
