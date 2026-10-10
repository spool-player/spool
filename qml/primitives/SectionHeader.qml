import QtQuick
import "../theme"

Item {
    id: root
    property string title: ""
    property url badgeIcon
    property string badgeText: ""
    implicitHeight: Math.max(titleText.implicitHeight, badge.visible ? badge.implicitHeight : 0)

    AppText {
        id: titleText
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(implicitWidth, parent.width - (badge.visible ? badge.width + Metrics.scaled(12) : 0))
        text: root.title
        font.pixelSize: Metrics.bodySizePx + 4
        font.weight: Font.DemiBold
        elide: Text.ElideRight
        maximumLineCount: 1
    }

    SourceBadge {
        id: badge
        anchors.left: titleText.right
        anchors.leftMargin: Metrics.scaled(12)
        anchors.verticalCenter: parent.verticalCenter
        iconUrl: root.badgeIcon
        text: root.badgeText
    }
}
