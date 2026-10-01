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
    readonly property color foreground: !enabled ? Theme.textMuted : kind === "primary" || kind === "blue"
                                                   ? Theme.accentText : kind === "danger" ? Theme.errorText :
                                                                                            Theme.textPrimary

    implicitWidth: Math.max(Metrics.scaled(132), buttonContent.implicitWidth + Metrics.scaled(34))
    implicitHeight: Metrics.controlHeightPx
    focusPolicy: Metrics.keyboardFocusActive ? Qt.StrongFocus : Qt.NoFocus

    background: Rectangle {
        radius: Theme.radiusMedium
        color: root.kind === "blue" ? Qt.darker(Theme.paletteBlue, !root.enabled ? 2.8 : tap.pressed ? 1.25 : 1) :
                                      !root.enabled ? Theme.bgPanel : root.kind === "primary" ? Theme.accent :
                                                                                                root.kind === "danger"
                                                                                                ? Theme.errorPanel :
                                                                                                  tap.pressed
                                                                                                  ? Theme.bgRaised :
                                                                                                    root.kind
                                                                                                    === "flat"
                                                                                                    ? "transparent" :
                                                                                                      Theme.bgPanel
        border.width: Metrics.keyboardFocusActive && root.activeFocus ? Theme.focusBorderWidth : root.pointerHovered ? Theme.hoverBorderWidth :
                                                                                                                       root.kind
                                                                                                                       === "flat"
                                                                                                                       ? 0 : Theme.hoverBorderWidth
        border.color: root.kind === "blue" ? (Metrics.keyboardFocusActive && root.activeFocus ? Theme.textPrimary : Qt.darker(Theme.paletteBlue,
                                                                                                                              1.4)) : !root.enabled
                                             ? Theme.border : Metrics.keyboardFocusActive && root.activeFocus
                                               ? Theme.textPrimary : root.pointerHovered ? Theme.borderStrong :
                                                                                           root.kind === "primary"
                                                                                           ? Theme.accentDim :
                                                                                             root.kind === "danger"
                                                                                             ? Theme.errorText :
                                                                                               Theme.border
        antialiasing: true
    }

    contentItem: Item {
        clip: true
        Row {
            id: buttonContent
            anchors.centerIn: parent
            spacing: Metrics.scaled(8)

            MaterialIcon {
                anchors.verticalCenter: parent.verticalCenter
                visible: root.iconName.length > 0
                name: root.iconName
                iconSize: Metrics.bodySizePx + 6
                iconColor: root.foreground
            }

            AppText {
                anchors.verticalCenter: parent.verticalCenter
                text: root.text
                color: root.foreground

                font.pixelSize: Metrics.bodySizePx
                font.weight: root.kind === "primary" || root.kind === "blue" || root.kind === "danger" ? Font.DemiBold :
                                                                                                         Font.Medium
                elide: Text.ElideRight
                maximumLineCount: 1
            }
        }
    }

    TapHandler {
        id: tap
        enabled: root.enabled
        onTapped: {
            InputKeys.focus(root)
            root.clicked()
        }
    }

    HoverHandler {
        id: hover
    }
}
