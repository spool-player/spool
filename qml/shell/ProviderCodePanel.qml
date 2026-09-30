pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Spool

Surface {
    id: root
    property string code: ""
    property string instructions: ""
    implicitHeight: content.implicitHeight + Metrics.scaled(34)
    baseColor: Theme.accentPanel
    ColumnLayout {
        id: content
        anchors.centerIn: parent
        width: parent.width - Metrics.scaled(40)
        spacing: Metrics.scaled(8)
        AppText {
            Layout.alignment: Qt.AlignHCenter
            text: root.code
            font.pixelSize: Metrics.scaled(32)
            font.weight: Font.Bold
            font.letterSpacing: Metrics.scaled(7)
        }
        SecondaryText {
            Layout.fillWidth: true
            text: root.instructions
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }
    }
}
