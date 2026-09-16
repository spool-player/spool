import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

FocusScope {
    id: root
    default property alias content: body.data
    property real preferredWidth: 620
    property real preferredHeight: -1
    property real padding: 24
    property color panelColor: Theme.bgPanel
    signal dismissed

    anchors.fill: parent
    focus: true

    Rectangle {
        anchors.fill: parent
        color: "#99000000"
        MouseArea {
            anchors.fill: parent
            onClicked: root.dismissed()
        }
    }

    Surface {
        anchors.centerIn: parent
        width: Math.min(parent.width - Metrics.scaled(96), Metrics.scaled(root.preferredWidth))
        height: Math.min(parent.height - Metrics.scaled(96), root.preferredHeight > 0 ? Metrics.scaled(
                                                                                            root.preferredHeight) :
                                                                                        body.implicitHeight
                                                                                        + Metrics.scaled(root.padding
                                                                                                         * 2))
        elevated: true
        baseColor: root.panelColor

        PopupShield {}

        ColumnLayout {
            id: body
            anchors.fill: parent
            anchors.margins: Metrics.scaled(root.padding)
            spacing: Metrics.scaled(14)
        }
    }
}
