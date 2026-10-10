pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Spool

FocusScope {
    id: root
    property var provider
    property var folders: provider && provider.arguments.configuration ? (provider.arguments.configuration.folders || []).slice(
                                                                             ) : []
    property string error: ""

    function addFolder(value) {
        const folder = String(value || "").trim()
        if (!folder.length)
            return
        if (folders.indexOf(folder) < 0)
            folders = folders.concat([folder])
        path.text = ""
        error = ""
    }
    function save() {
        if (!folders.length) {
            error = "Choose at least one folder. Nothing is added automatically."
            return
        }
        provider.complete({
                              account: "local-library",
                              label: "Local files",
                              configuration: {
                                  folders: folders
                              }
                          })
    }
    function activate() {
        const item = Window.activeFocusItem
        if (item && typeof item.activate === "function")
            item.activate()
        else if (item && typeof item.clicked === "function")
            item.clicked()
    }
    Component.onCompleted: Qt.callLater(() => path.focusRow())

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - Metrics.pageMarginPx * 2, Metrics.scaled(720))
        spacing: Metrics.scaled(12)
        AppText {
            text: "Local files"
            font.pixelSize: Metrics.titleSizePx
            font.weight: Font.DemiBold
        }
        SecondaryText {
            Layout.fillWidth: true
            text: "Choose the folders to combine into one library. Subfolders are included; overlapping folders do not duplicate files."
            wrapMode: Text.WordWrap
        }
        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, root.height * 0.35)
            clip: true
            model: root.folders
            delegate: RowLayout {
                required property string modelData
                required property int index
                width: ListView.view.width
                SecondaryText {
                    Layout.fillWidth: true
                    text: modelData
                    elide: Text.ElideMiddle
                }
                ActionButton {
                    text: "Remove"
                    kind: "flat"
                    onClicked: root.folders = root.folders.filter((_, i) => i !== index)
                }
            }
        }
        TextFieldRow {
            id: path
            Layout.fillWidth: true
            label: "Folder path"
            onAccepted: root.addFolder(text)
        }
        RowLayout {
            ActionButton {
                text: "Add folder"
                enabled: path.text.trim().length > 0
                onClicked: root.addFolder(path.text)
            }
            ActionButton {
                text: "Browse…"
                onClicked: folderDialog.active = true
            }
        }
        SecondaryText {
            Layout.fillWidth: true
            visible: root.error.length > 0
            text: root.error
            color: Theme.errorText
            wrapMode: Text.WordWrap
        }
        ActionButton {
            Layout.fillWidth: true
            text: root.provider && root.provider.role === "settings" ? "Save folders" : "Add library"
            kind: "primary"
            onClicked: root.save()
        }
    }
    Loader {
        id: folderDialog
        active: false
        source: "DesktopFolderDialog.qml"
        onLoaded: {
            item.title = "Choose a media folder"
            item.open()
        }
        Connections {
            target: folderDialog.item
            function onFolderSelected(folder) {
                root.addFolder(folder)
                folderDialog.active = false
            }
            function onDismissed() {
                folderDialog.active = false
            }
        }
    }
}
