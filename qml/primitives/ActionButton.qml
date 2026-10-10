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
    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.focusable: enabled
    Accessible.focused: activeFocus
    Accessible.pressed: tap.pressed
    Accessible.onPressAction: if (enabled)
                                  clicked()

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
        Item {
            id: buttonContent
            anchors.centerIn: parent
            implicitWidth: buttonLabel.implicitWidth + (buttonIcon.visible ? buttonIcon.width + Metrics.scaled(8) : 0)
            width: Math.min(implicitWidth, Math.max(0, parent.width - Metrics.scaled(34)))
            height: Math.max(buttonLabel.implicitHeight, buttonIcon.visible ? buttonIcon.height : 0)

            MaterialIcon {
                id: buttonIcon
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                visible: root.iconName.length > 0
                name: root.iconName
                iconSize: Metrics.bodySizePx + 6
                iconColor: root.foreground
                Accessible.ignored: true
            }

            AppText {
                id: buttonLabel
                anchors.left: buttonIcon.visible ? buttonIcon.right : parent.left
                anchors.leftMargin: buttonIcon.visible ? Metrics.scaled(8) : 0
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: root.text
                color: root.foreground

                font.pixelSize: Metrics.bodySizePx
                font.weight: root.kind === "primary" || root.kind === "blue" || root.kind === "danger" ? Font.DemiBold :
                                                                                                         Font.Medium
                elide: Text.ElideRight
                maximumLineCount: 1
                Accessible.ignored: true
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
