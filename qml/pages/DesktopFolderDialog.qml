import QtQuick
import QtQuick.Dialogs

Item {
    id: root
    property string title: "Choose custom mpv directory"
    property bool useSystemDialog: Qt.platform.os !== "osx" || Settings.values["appearance/systemFileDialogs"] !== false

    signal folderSelected(url folder)
    signal dismissed

    function open() {
        if (picker.item && picker.item.visible)
            return
        const options = useSystemDialog ? 0 : FolderDialog.DontUseNativeDialog
        if (!picker.item || picker.item.options !== options) {
            const currentFolder = picker.item ? picker.item.currentFolder : null
            picker.active = false
            picker.active = true
            picker.item.options = options
            if (currentFolder)
                picker.item.currentFolder = currentFolder
        }
        picker.item.open()
    }

    Loader {
        id: picker
        active: false
        sourceComponent: FolderDialog {
            title: root.title
            onAccepted: root.folderSelected(selectedFolder)
            onRejected: root.dismissed()
        }
    }
}
