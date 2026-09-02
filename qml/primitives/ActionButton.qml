import QtQuick
import QtQuick.Templates as T
import "../theme"

T.Control {
    id: root
    property string kind: "secondary"
    property string iconName: ""
    property bool pointerHovered: hover.hovered
    property string text: ""
    signal clicked

    // The label is measured here rather than by reading the Row inside
    // contentItem. contentItem is one of Control's deferred properties, and an
    // id anywhere under a deferred property forces the assignment to happen
    // immediately -- which changes when the object is built, and which the QML
    // type compiler refuses outright (deferred-property-id). Measuring the
    // text directly keeps the sizing identical without reaching into it.
    readonly property real iconAdvance: root.iconName.length > 0 ? labelMetrics.iconSize + labelRow.spacing : 0

    implicitWidth: Math.max(Metrics.scaled(132), labelMetrics.width + root.iconAdvance + Metrics.scaled(34))
    implicitHeight: Metrics.controlHeightPx
    focusPolicy: Metrics.keyboardFocusActive ? Qt.StrongFocus : Qt.NoFocus

    background: Rectangle {
        radius: Theme.radiusMedium
        color: root.kind === "primary" ? (tap.pressed ? Theme.accentDim : (root.pointerHovered || (
                                                                               Metrics.keyboardFocusActive
                                                                               && root.activeFocus) ? Theme.accent :
                                                                                                      Theme.accentDim)) :
                                         root.kind === "danger" ? (tap.pressed ? Theme.bgRaised : Theme.errorPanel) :
                                                                  tap.pressed ? Theme.bgRaised : root.kind === "flat"
                                                                                ? "transparent" : Theme.bgPanel
        border.width: Metrics.keyboardFocusActive && root.activeFocus ? Theme.focusBorderWidth : root.pointerHovered ? Theme.hoverBorderWidth :
                                                                                                                       root.kind
                                                                                                                       === "flat"
                                                                                                                       ? 0 : Theme.hoverBorderWidth
        border.color: Metrics.keyboardFocusActive && root.activeFocus ? Theme.textPrimary : root.pointerHovered ? Theme.borderStrong :
                                                                                                                  root.kind
                                                                                                                  === "primary"
                                                                                                                  ? Theme.accentDim :
                                                                                                                    root.kind
                                                                                                                    === "danger"
                                                                                                                    ? Theme.errorText :
                                                                                                                      Theme.border
        antialiasing: true
    }

    // Sizing constants the measurement above and the visible Row below must
    // agree on, so they cannot drift apart.
    QtObject {
        id: labelRow
        readonly property real spacing: Metrics.scaled(8)
    }

    TextMetrics {
        id: labelMetrics
        readonly property real iconSize: Metrics.bodySizePx + 6
        font.family: Typography.sans
        font.pixelSize: Metrics.bodySizePx
        font.weight: root.kind === "primary" || root.kind === "danger" ? Font.DemiBold : Font.Medium
        text: root.text
    }

    contentItem: Item {
        clip: true
        Row {
            anchors.centerIn: parent
            spacing: labelRow.spacing

            MaterialIcon {
                anchors.verticalCenter: parent.verticalCenter
                visible: root.iconName.length > 0
                name: root.iconName
                iconSize: labelMetrics.iconSize
                iconColor: root.enabled ? Theme.textPrimary : Theme.textDisabled
            }

            AppText {
                anchors.verticalCenter: parent.verticalCenter
                text: root.text
                color: root.enabled ? (root.kind === "danger" ? Theme.errorText : Theme.textPrimary) :
                                      Theme.textDisabled

                font.pixelSize: Metrics.bodySizePx
                font.weight: root.kind === "primary" || root.kind === "danger" ? Font.DemiBold : Font.Medium
                elide: Text.ElideRight
                maximumLineCount: 1
            }
        }
    }

    TapHandler {
        id: tap
        onTapped: {
            InputKeys.focus(root)
            root.clicked()
        }
    }

    HoverHandler {
        id: hover
    }
}
