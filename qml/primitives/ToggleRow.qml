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
        SwitchIndicator {
            Layout.alignment: Qt.AlignVCenter
            checked: root.checked
            focused: root.rowFocus
            animate: root.animateChange
        }
    ]

    Timer {
        id: animationReset
        interval: 180
        onTriggered: root.animateChange = false
    }
}
