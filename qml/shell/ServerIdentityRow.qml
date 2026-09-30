pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Spool

Surface {
    id: root
    property string serverName: ""
    property string serverAddress: ""
    signal changeRequested
    implicitHeight: Math.max(Metrics.controlHeightPx, content.implicitHeight + Metrics.scaled(16))
    RowLayout {
        id: content
        anchors.fill: parent
        anchors.margins: Metrics.scaled(8)
        spacing: Metrics.scaled(10)
        MaterialIcon {
            name: "dns"
            iconColor: Theme.textMuted
            iconSize: Metrics.iconSizePx
        }
        AppText {
            Layout.maximumWidth: root.width * 0.38
            text: root.serverName
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        SecondaryText {
            Layout.fillWidth: true
            text: root.serverAddress
            elide: Text.ElideMiddle
        }
        ActionButton {
            text: "Change"
            kind: "flat"
            onClicked: root.changeRequested()
        }
    }
}
