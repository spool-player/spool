import QtQuick
import "../theme"

Item {
    id: root

    property var info: null
    property int rightInset: Metrics.scaled(12)
    readonly property bool hasContent: infoLineRow.implicitWidth > 0
    // PT Root UI's digit/cap ink center is 9.85% of an em above its typo line center.
    readonly property real textInkCenterOffset: -Metrics.metaSizePx * 0.0985

    implicitWidth: infoLineRow.implicitWidth + rightInset
    implicitHeight: Metrics.scaled(40)

    component InfoSegment: Row {
        property string iconName
        property string text
        visible: text.length > 0
        spacing: Metrics.scaled(6)

        MaterialIcon {
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: root.textInkCenterOffset
            name: parent.iconName
            iconSize: Metrics.metaSizePx + 4
            iconColor: Theme.textSecondary
        }

        SecondaryText {
            anchors.verticalCenter: parent.verticalCenter
            text: parent.text
            color: Theme.textPrimary
            font.pixelSize: Metrics.metaSizePx
            maximumLineCount: 1
        }
    }

    Row {
        id: infoLineRow
        readonly property var values: root.info || ({})
        readonly property string resolutionText: String(values.resolution || "")
        anchors.right: parent.right
        anchors.rightMargin: root.rightInset
        anchors.verticalCenter: parent.verticalCenter
        spacing: Metrics.scaled(16)

        InfoSegment {
            iconName: "calendar_today"
            text: String(infoLineRow.values.date || "")
        }

        InfoSegment {
            iconName: "schedule"
            text: String(infoLineRow.values.runtime || "")
        }

        Rectangle {
            visible: infoLineRow.resolutionText.length > 0
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: -Metrics.scaled(1) + root.textInkCenterOffset
            width: resolutionLabel.implicitWidth + Metrics.scaled(12)
            // Even padding keeps NativeRendering from snapping a half-pixel baseline upward.
            height: resolutionLabel.implicitHeight + 2 * Metrics.scaled(3)
            radius: Theme.radiusTiny
            color: "transparent"
            border.width: Theme.hoverBorderWidth
            border.color: Theme.textSecondary

            SecondaryText {
                id: resolutionLabel
                anchors.centerIn: parent
                anchors.verticalCenterOffset: Metrics.scaled(2) - 0.5
                text: infoLineRow.resolutionText
                color: Theme.textPrimary
                font.pixelSize: Metrics.metaSizePx - 1
                font.weight: Font.DemiBold
                maximumLineCount: 1
            }
        }

        SecondaryText {
            text: String(infoLineRow.values.hdr || "")
            visible: text.length > 0
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.textPrimary
            font.pixelSize: Metrics.metaSizePx
            maximumLineCount: 1
        }

        InfoSegment {
            iconName: "graphic_eq"
            text: String(infoLineRow.values.audio || "")
        }

        InfoSegment {
            iconName: "subtitles"
            text: String(infoLineRow.values.subs || "")
        }
    }
}
