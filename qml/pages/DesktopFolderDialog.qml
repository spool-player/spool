import QtQuick
import QtQuick.Dialogs

Item {
    id: root
    property alias title: dialog.title

    signal folderSelected(url folder)
    signal dismissed

    function open() {
        dialog.open()
    }

    FolderDialog {
        id: dialog
        title: "Choose custom mpv directory"
        onAccepted: root.folderSelected(selectedFolder)
        onRejected: root.dismissed()
    }
}
