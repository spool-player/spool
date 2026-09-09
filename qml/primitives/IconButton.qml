import QtQuick
import QtQuick.Templates as T
import "../theme"

T.Control {
    id: root
    property string iconText: ""
    property string iconName: ""
    property bool selected: false
    property bool checked: false
    property bool railStyle: false
    property bool chromeless: false
    property bool pointerHovered: hover.hovered && Metrics.pointerActive
    // Touch targets may grow without enlarging the glyph. Size from the
    // visual control scale, bounded by both dimensions of the actual button.
    property real iconRatio: railStyle ? 0.545 : 0.5
    property string accessibleName: ""
    // A click normally leaves the keyboard selection on what was clicked, so
    // a remote or a keyboard carries on from there. A button that navigates
    // away wants the opposite: the selection belongs to the page it opens,
    // and a ring painted on the button first only flashes on the way out --
    // it lands on mouse press, so it is on screen for the whole time a finger
    // rests on the button before the page even starts changing. Focusing it
    // by keyboard still works: that goes through forceActiveFocus, which no
    // focus policy gates.
    property bool focusOnClick: true
    property alias acceptedButtons: tap.acceptedButtons
    signal clicked

    width: Math.max(Metrics.touchTargetPx, Metrics.scaled(44))
    height: width
    focusPolicy: Metrics.keyboardFocusActive && root.focusOnClick ? Qt.StrongFocus : Qt.NoFocus

    background: Rectangle {
        radius: Theme.radiusSmall
        color: root.chromeless ? (tap.pressed ? Theme.bgHover : "transparent") : tap.pressed ? Theme.bgHover : root.railStyle
                                                                                               && root.selected
                                                                                               ? Theme.accentPanel :
                                                                                                 root.selected
                                                                                                 ? Theme.bgPanel :
                                                                                                   "transparent"
        border.width: root.chromeless ? 0 : Metrics.keyboardFocusActive && root.activeFocus ? Theme.focusBorderWidth :
                                                                                              root.selected
                                                                                              || root.pointerHovered
                                                                                              ? Theme.hoverBorderWidth :
                                                                                                0
        border.color: Metrics.keyboardFocusActive && root.activeFocus ? Theme.textPrimary : root.selected ? Theme.accent :
                                                                                                            Theme.borderStrong
        antialiasing: true

        Rectangle {
            anchors.fill: parent
            anchors.margins: -Metrics.scaled(4)
            radius: parent.radius + Metrics.scaled(4)
            color: "transparent"
            border.width: !root.chromeless && root.railStyle && Metrics.keyboardFocusActive && root.activeFocus
                          ? Theme.focusBorderWidth : 0
            border.color: Theme.accent
            opacity: 0.55
        }
    }

    contentItem: Item {
        MaterialIcon {
            anchors.centerIn: parent
            visible: root.iconName.length > 0
            name: root.iconName
            iconSize: Math.round(Math.min(root.width, root.height, Metrics.scaled(44)) * root.iconRatio)
            iconColor: Metrics.keyboardFocusActive && root.activeFocus || root.selected ? Theme.accent :
                                                                                          Theme.textSecondary
        }

        AppText {
            anchors.fill: parent
            visible: root.iconName.length === 0
            text: root.iconText
            font.pixelSize: Metrics.scaled(20)
            font.weight: Font.DemiBold
            color: Metrics.keyboardFocusActive && root.activeFocus ? Theme.textPrimary : root.selected ? Theme.accent :
                                                                                                         Theme.textSecondary
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    TapHandler {
        id: tap
        onTapped: {
            if (root.focusOnClick)
                InputKeys.focus(root)
            root.clicked()
        }
    }

    HoverHandler {
        id: hover
    }
}
