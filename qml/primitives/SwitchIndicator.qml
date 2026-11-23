import QtQuick
import "../theme"

// The on/off track drawn beside a setting. Focus outlines it in the text
// colour, which stays visible against an accent-filled track; off draws an
// input-strength edge so the empty track still reads as a control.
Rectangle {
    id: track

    property bool checked: false
    property bool focused: false
    property bool animate: false
    readonly property int inset: Math.max(2, Metrics.scaled(3))

    implicitWidth: Metrics.scaled(52)
    implicitHeight: Metrics.scaled(30)
    radius: height / 2
    color: checked ? Theme.accent : Theme.inputFill
    border.width: focused ? Theme.focusBorderWidth : Theme.hoverBorderWidth
    border.color: focused ? Theme.textPrimary : checked ? Theme.accent : Theme.inputBorder
    antialiasing: true

    Rectangle {
        y: (parent.height - height) / 2
        x: track.checked ? parent.width - width - track.inset : track.inset
        width: parent.height - track.inset * 2
        height: width
        radius: width / 2
        color: track.checked ? Theme.accentText : Theme.textSecondary
        antialiasing: true

        Behavior on x {
            enabled: track.animate && !Theme.reducedMotion
            NumberAnimation {
                duration: 120
                easing.type: Easing.OutCubic
            }
        }
    }
}
