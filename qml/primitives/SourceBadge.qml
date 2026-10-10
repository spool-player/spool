import QtQuick
import "../theme"

// Where something comes from, when there is more than one place it could:
// a provider's mark beside a server's name, as a quiet pill that reads over
// artwork as well as over the page.
Rectangle {
    id: root

    property url iconUrl
    property string text: ""
    // Over artwork the pill needs its own dark ground; on the page it does not.
    property bool overlay: false
    property int iconSize: Metrics.scaled(18)
    property real maximumWidth: Metrics.scaled(260)

    visible: text.length > 0
    implicitWidth: content.implicitWidth + Metrics.scaled(overlay ? 16 : 14)
    implicitHeight: Math.max(iconSize, label.implicitHeight) + Metrics.scaled(10)
    radius: height / 2
    color: overlay ? "#C8101010" : Theme.bgPanel
    border.width: 1
    border.color: overlay ? "#33FFFFFF" : Theme.border
    antialiasing: true

    Row {
        id: content
        anchors.centerIn: parent
        spacing: Metrics.scaled(7)

        Image {
            anchors.verticalCenter: parent.verticalCenter
            width: root.iconSize
            height: root.iconSize
            visible: status === Image.Ready
            source: root.iconUrl
            sourceSize.width: width * 2
            sourceSize.height: height * 2
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            smooth: true
        }

        AppText {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(0, Math.min(implicitWidth, Metrics.scaled(220), root.maximumWidth - Metrics.scaled(
                                            root.overlay ? 16 : 14) - root.iconSize - content.spacing))
            text: root.text
            color: Theme.textPrimary
            font.pixelSize: Metrics.metaSizePx
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            maximumLineCount: 1
        }
    }
}
