import QtQuick
import "shell"

FocusScope {
    id: root
    width: 1920
    height: 1080
    focus: true

    Connections {
        target: App
        function onAggressiveMemoryPressure() {
            Qt.inputMethod.hide()
        }
    }

    AppShell {
        objectName: "appShell"
        anchors.fill: parent
    }
}
