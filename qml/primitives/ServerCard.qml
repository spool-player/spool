import QtQuick
import QtQuick.Layouts
import "../theme"

// One server, as a row to pick: its name and address.
FocusScope {
    id: root

    property string title: ""
    property string serverAddress: ""
    property bool focused: activeFocus

    signal accepted

    function activate() {
        accepted()
    }

    implicitHeight: Math.max(Metrics.touchTargetPx, Metrics.scaled(64))
    focusPolicy: Qt.StrongFocus

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusMedium
        color: root.focused ? Theme.accentPanel : hover.hovered && Metrics.pointerActive ? Theme.bgHover :
                                                                                           Theme.bgRaised

        border.width: root.focused ? Theme.focusBorderWidth : 1
        border.color: root.focused ? Theme.accent : Theme.border
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Metrics.scaled(16)
        anchors.rightMargin: Metrics.scaled(14)
        spacing: Metrics.scaled(14)

        MaterialIcon {
            name: "dns"
            iconSize: Metrics.scaled(22)
            iconColor: root.focused ? Theme.accent : Theme.textMuted
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Metrics.scaled(2)

            AppText {
                Layout.fillWidth: true
                text: root.title
                font.pixelSize: Metrics.bodySizePx + Metrics.scaled(2)
                font.weight: Font.Medium
                maximumLineCount: 1
                elide: Text.ElideRight
            }

            SecondaryText {
                Layout.fillWidth: true
                visible: root.serverAddress.length > 0 && root.serverAddress !== root.title
                text: root.serverAddress
                color: Theme.textMuted
                font.pixelSize: Metrics.metaSizePx
                maximumLineCount: 1
                elide: Text.ElideRight
            }
        }
    }

    TapHandler {
        onTapped: root.accepted()
    }

    HoverHandler {
        id: hover
    }
}
