import QtQuick
import QtQuick.Layouts
import "../theme"

SettingRow {
    id: root

    property var options: []
    property int currentIndex: 0
    readonly property string selectedText: options.length > 0 ? String(options[Math.max(0, Math.min(options.length - 1,
                                                                                                    currentIndex))]) :
                                                                ""

    signal selected(int index, string value)
    signal opened

    valueText: selectedText
    valueTextVisible: false

    function move(direction) {
        if (options.length <= 0)
            return false
        opened()
        return true
    }

    onClicked: opened()

    trailing: Rectangle {
        readonly property int sidePadding: Metrics.scaled(16)

        Layout.alignment: Qt.AlignVCenter
        // Hug the value instead of always reserving the widest pill: short
        // answers like "Blue" should not sit in a field of empty chrome. The
        // row layout reserves this space so long labels cannot run under it.
        Layout.preferredWidth: Math.min(root.availableWidth * 0.5, Math.max(Metrics.scaled(120), Math.min(valueLabel.implicitWidth
                                                                                                          + dropdownIcon.width
                                                                                                          + sidePadding
                                                                                                          * 2 + Metrics.scaled(
                                                                                                              10), Math.max(
                                                                                                              Metrics.scaled(
                                                                                                                  180), root.width
                                                                                                              * 0.46))))
        Layout.preferredHeight: Math.max(Metrics.scaled(40), Metrics.controlHeightPx)
        radius: Theme.radiusMedium
        color: root.rowFocus ? Theme.accentPanel : Theme.bgRaised
        border.width: root.rowFocus ? Theme.focusBorderWidth : Theme.hoverBorderWidth
        border.color: root.rowFocus ? Theme.accent : Theme.borderStrong
        antialiasing: true

        AppText {
            id: valueLabel
            anchors.left: parent.left
            anchors.right: dropdownIcon.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.leftMargin: parent.sidePadding
            anchors.rightMargin: Metrics.scaled(10)
            text: root.selectedText
            color: Theme.textPrimary
            font.pixelSize: Metrics.metaSizePx
            font.weight: Font.DemiBold
            horizontalAlignment: Text.AlignLeft
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        MaterialIcon {
            id: dropdownIcon
            anchors.right: parent.right
            anchors.rightMargin: parent.sidePadding - Metrics.scaled(2)
            anchors.verticalCenter: parent.verticalCenter
            name: "expand_more"
            iconSize: Math.max(20, Metrics.iconSizePx)
            iconColor: root.options.length > 0 ? Theme.textPrimary : Theme.textDisabled
        }
    }
}
