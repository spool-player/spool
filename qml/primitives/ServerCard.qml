import QtQuick
import QtQuick.Layouts
import "../theme"

// One server, as a row to pick: its name and address.
FocusScope {
    id: root

    property string title: ""
    property string serverAddress: ""
    property bool focused: activeFocus
    property string providerName: ""
    property string providerId: ""
    property url providerIcon
    property string providerVersion: ""
    readonly property int brandingHeight: providerName.length > 0 ? Metrics.scaled(30) : 0

    signal accepted

    function activate() {
        accepted()
    }

    implicitHeight: Math.max(Metrics.touchTargetPx, Metrics.scaled(64)) + brandingHeight
    focusPolicy: Qt.StrongFocus
    Accessible.role: Accessible.Button
    Accessible.name: title
    Accessible.description: [serverAddress, providerName.length > 0 ? providerName + (providerVersion.length > 0 ? ", installed provider version "
                                                                                                                   + providerVersion :
                                                                                                                   "") : ""].filter(
        part => part.length > 0).join(". ")
    Accessible.onPressAction: root.accepted()

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
        anchors.bottomMargin: root.brandingHeight

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

    ProviderIcon {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: Metrics.scaled(12)
        anchors.bottomMargin: Metrics.scaled(6)
        width: Metrics.scaled(20)
        height: width
        visible: root.brandingHeight > 0
        source: root.providerIcon
        name: root.providerName
        seed: root.providerId
        Accessible.ignored: true
    }
    SecondaryText {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: Metrics.scaled(14)
        anchors.bottomMargin: Metrics.scaled(6)
        width: Math.max(0, parent.width - Metrics.scaled(60))
        visible: root.providerVersion.length > 0
        text: root.providerVersion
        horizontalAlignment: Text.AlignRight
        font.pixelSize: Metrics.metaSizePx
        elide: Text.ElideRight
        Accessible.ignored: true
    }

    TapHandler {
        onTapped: root.accepted()
    }

    HoverHandler {
        id: hover
    }
}
