import QtQuick
import QtQuick.Layouts
import "../theme"

SettingRow {
    id: root
    property bool checked: false
    property bool animateChange: false
    signal toggled(bool checked)

    // The switch itself says on or off; the word next to it only repeats it.
    valueTextVisible: false
    Accessible.role: Accessible.CheckBox
    Accessible.checkable: true
    Accessible.checked: checked
    Accessible.onToggleAction: if (enabled)
                                   toggle()

    function toggle() {
        animateChange = true
        animationReset.restart()
        toggled(!checked)
    }

    onClicked: toggle()

    trailing: [
        Rectangle {
            id: track
            readonly property int inset: Math.max(2, Metrics.scaled(3))

            Layout.alignment: Qt.AlignVCenter
            implicitWidth: Metrics.scaled(52)
            implicitHeight: Metrics.scaled(30)
            radius: height / 2
            color: root.checked ? Theme.accent : Theme.bgRaised
            border.width: root.rowFocus ? Theme.focusBorderWidth : Theme.hoverBorderWidth
            border.color: root.rowFocus ? Theme.textPrimary : root.checked ? Theme.accent : Theme.borderStrong
            antialiasing: true

            Rectangle {
                y: (parent.height - height) / 2
                x: root.checked ? parent.width - width - track.inset : track.inset
                width: parent.height - track.inset * 2
                height: width
                radius: width / 2
                color: root.checked ? Theme.accentText : Theme.textSecondary
                antialiasing: true

                Behavior on x {
                    enabled: root.animateChange && !Theme.reducedMotion
                    NumberAnimation {
                        duration: 120
                        easing.type: Easing.OutCubic
                    }
                }
            }
        }
    ]

    Timer {
        id: animationReset
        interval: 180
        onTriggered: root.animateChange = false
    }
}
