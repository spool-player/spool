import QtQuick
import QtQuick.Layouts
import "../theme"

SettingRow {
    id: root

    property var options: []
    property int currentIndex: 0
    // Set while this row's option list is open, so the field reads as open.
    property bool expanded: false
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
        id: field
        readonly property int sidePadding: Metrics.scaled(14)

        Layout.alignment: Qt.AlignVCenter
        // Hug the value instead of always reserving the widest field: short
        // answers like "Blue" should not sit in a field of empty chrome. The
        // row layout reserves this space so long labels cannot run under it.
        Layout.preferredWidth: Math.min(root.availableWidth * 0.5, Math.max(Metrics.scaled(140), Math.min(valueLabel.implicitWidth
                                                                                                          + chevron.width
                                                                                                          + sidePadding
                                                                                                          * 2 + Metrics.scaled(
                                                                                                              12), Math.max(
                                                                                                              Metrics.scaled(
                                                                                                                  180), root.width
                                                                                                              * 0.46))))
        Layout.preferredHeight: Math.max(Metrics.scaled(42), Metrics.controlHeightPx)
        radius: Theme.radiusLarge
        color: root.rowFocus || root.expanded ? Theme.inputFillActive : Theme.inputFill
        border.width: root.rowFocus || root.expanded ? Theme.focusBorderWidth : Theme.hoverBorderWidth
        border.color: root.rowFocus || root.expanded ? Theme.accent : fieldHover.hovered ? Theme.inputBorderHover :
                                                                                           Theme.inputBorder
        antialiasing: true

        HoverHandler {
            id: fieldHover
        }

        AppText {
            id: valueLabel
            anchors.left: parent.left
            anchors.right: chevron.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.leftMargin: parent.sidePadding
            anchors.rightMargin: Metrics.scaled(8)
            text: root.selectedText
            color: Theme.textPrimary
            font.pixelSize: Metrics.metaSizePx + Metrics.scaled(1)
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignLeft
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        MaterialIcon {
            id: chevron
            anchors.right: parent.right
            anchors.rightMargin: parent.sidePadding - Metrics.scaled(4)
            anchors.verticalCenter: parent.verticalCenter
            name: "expand_more"
            iconSize: Math.max(20, Metrics.iconSizePx)
            iconColor: root.options.length === 0 ? Theme.textDisabled : root.rowFocus || root.expanded ? Theme.accent :
                                                                                                         Theme.textSecondary
            rotation: root.expanded ? 180 : 0

            Behavior on rotation {
                enabled: !Theme.reducedMotion
                NumberAnimation {
                    duration: 140
                    easing.type: Easing.OutCubic
                }
            }
        }
    }
}
