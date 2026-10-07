pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

OverlayDialog {
    id: root
    objectName: "downloadsDialog"
    preferredWidth: 680
    preferredHeight: 680
    padding: 20

    property bool destinationExpanded: false
    property bool inputActive: true
    property bool choosingAgain: false
    property string selectedJobId: ""
    property var deletionJob: null
    property bool cancelling: false
    readonly property string itemId: Downloads.selectionItemId
    readonly property var itemStatus: {
        const jobs = Downloads.jobs
        return itemId ? Downloads.statusFor(itemId) : ({})
    }
    readonly property bool selecting: Downloads.enabled && !!itemId && (!itemStatus.id || choosingAgain)
    readonly property var options: selecting ? Sources.downloadOptions(itemId) : []
    readonly property var visibleJobs: itemId ? Downloads.jobs.filter(job => job.itemId === itemId) : Downloads.jobs
    readonly property var selectedJob: visibleJobs.find(job => job.id === selectedJobId) || visibleJobs[Math.max(0,
                                                                                                                 list.currentIndex)]
                                       || ({})
    readonly property bool activeJob: selectedJob.state === "preparing" || selectedJob.state === "downloading"
    readonly property string selectedState: String(selectedJob.state || "")
    readonly property bool folderControls: Downloads.enabled && Downloads.canChooseFolder && (!Downloads.mobile
                                                                                              || destinationExpanded)
    readonly property bool editingDestination: destinationExpanded && folderControls
    readonly property bool folderBrowsing: editingDestination && Platform.isWebOS
    readonly property var rows: folderBrowsing ? Downloads.folderEntries(folderPath.text) : selecting ? options :
                                                                                                        visibleJobs

    property bool reconcilingRows: false

    ListModel {
        id: downloadRows
    }

    // Keep delegates and selection alive while byte counts change.
    function syncRows() {
        const keepId = selectedJobId
        const previousIndex = list.currentIndex
        reconcilingRows = true
        for (let index = 0; index < rows.length; ++index) {
            const rowKey = selecting ? "option:" + index : String(rows[index].id)
            let existing = index
            while (existing < downloadRows.count && downloadRows.get(existing).entryKey !== rowKey)
                ++existing
            if (existing === downloadRows.count)
                downloadRows.insert(index, {
                                        entryKey: rowKey,
                                        record: rows[index]
                                    })
            else {
                if (existing !== index)
                    downloadRows.move(existing, index, 1)
                downloadRows.setProperty(index, "record", rows[index])
            }
        }
        if (downloadRows.count > rows.length)
            downloadRows.remove(rows.length, downloadRows.count - rows.length)
        const selectedIndex = selecting ? -1 : rows.findIndex(job => job.id === keepId)
        list.currentIndex = rows.length > 0 ? selectedIndex >= 0 ? selectedIndex : Math.max(0, Math.min(previousIndex,
                                                                                                        rows.length
                                                                                                        - 1)) : -1
        selectedJobId = !selecting && list.currentIndex >= 0 ? rows[list.currentIndex].id : ""
        reconcilingRows = false
    }

    function conciseError(error) {
        return String(error || "").replace(/\s+/g, " ").trim().slice(0, 160)
    }

    function bytes(value) {
        let amount = Math.max(0, Number(value) || 0)
        const units = ["B", "KiB", "MiB", "GiB", "TiB"]
        let unit = 0
        while (amount >= 1024 && unit < units.length - 1) {
            amount /= 1024;
            ++unit
        }
        return (unit === 0 ? Math.round(amount) : amount.toFixed(1)) + " " + units[unit]
    }

    function statusText(job) {
        const received = bytes(job.received)
        switch (job.state) {
        case "preparing":
            return "Preparing download…"
        case "downloading":
            return Number(job.total) > 0 ? Math.min(100, Math.floor(Number(job.received) / Number(job.total) * 100))
                                           + "% · " + received + " of " + bytes(job.total) : received
                                           + " received · Downloading…"
        case "complete":
            return "Downloaded · " + received
        case "failed":
            return "Failed"
        case "cancelled":
            return "Cancelled"
        default:
            return ""
        }
    }

    function focusEntry() {
        if (!inputActive)
            return
        if (destinationExpanded && folderControls) {
            InputKeys.focus(chooseFolderButton)
        } else if (selecting && options.length === 1) {
            InputKeys.focus(downloadButton)
        } else if (list.count > 0) {
            list.currentIndex = Math.max(0, Math.min(list.currentIndex, list.count - 1))
            if (!selecting && visibleJobs[list.currentIndex])
                selectedJobId = visibleJobs[list.currentIndex].id
            InputKeys.focus(list)
        } else {
            InputKeys.focus(closeButton)
        }
    }

    function focusJobAction() {
        if (!inputActive)
            return
        InputKeys.focus(activeJob ? cancelButton : selectedJob.state === "complete" ? playButton : Downloads.enabled && (
                                                                                          selectedJob.state
                                                                                          === "failed"
                                                                                          || selectedJob.state
                                                                                          === "cancelled")
                                                                                      ? retryButton : closeButton)
    }

    function chooseDestination() {
        if (!Downloads.canChooseFolder)
            return
        if (Downloads.mobile) {
            Downloads.chooseFolder()
        } else if (Platform.isWebOS) {
            destinationExpanded = true
            Qt.callLater(() => folderPath.focusRow())
        } else if (folderDialog.item) {
            folderDialog.item.open()
        } else {
            folderDialog.active = true
        }
    }

    function savePath() {
        const path = folderPath.text.trim()
        Downloads.setDestination(path.startsWith("/") ? "file://" + path.split("/").map(encodeURIComponent).join("/") :
                                                        path)
    }

    function routeKey(key, phase, repeat) {
        if (deletionJob)
            return deletionDialog.item ? deletionDialog.item.routeKey(key, phase, repeat) : true
        const current = root.Window.window ? root.Window.window.activeFocusItem : null
        if (InputKeys.isBack(key, false, false)) {
            if (phase === "release")
                back()
            return true
        }
        if (InputKeys.isAccept(key))
            return !InputKeys.isTextInputItem(current)
        if (!InputKeys.isDirection(key))
            return false
        if (InputKeys.isTextInputItem(current))
            return false
        if (phase !== "press")
            return true
        if (list.activeFocus && (key === Qt.Key_Up || key === Qt.Key_Down)) {
            const nextIndex = list.currentIndex + (key === Qt.Key_Down ? 1 : -1)
            if (nextIndex >= 0 && nextIndex < list.count) {
                list.currentIndex = nextIndex
                list.positionViewAtIndex(nextIndex, ListView.Contain)
                return true
            }
        }
        if (current) {
            const forward = key === Qt.Key_Down || key === Qt.Key_Right
            const next = current.nextItemInFocusChain(forward)
            if (next && root.contains(root.mapFromItem(next, 0, 0)))
                InputKeys.focus(next)
        }
        return true
    }

    function activate() {
        if (deletionJob) {
            if (deletionDialog.item)
                deletionDialog.item.activate()
            return
        }
        const current = root.Window.window ? root.Window.window.activeFocusItem : null
        if (list.activeFocus && list.currentItem)
            list.currentItem.activated()
        else if (current && typeof current.activate === "function")
            current.activate()
        else if (current && typeof current.clicked === "function")
            current.clicked()
    }

    function back() {
        if (deletionJob) {
            deletionJob = null
            Qt.callLater(() => InputKeys.focus(cancelling ? cancelButton : removeButton))
        } else if (folderPath.editing) {
            Qt.inputMethod.hide()
            InputKeys.focus(chooseFolderButton)
            return true
        } else if (choosingAgain) {
            choosingAgain = false
            Qt.callLater(focusEntry)
        } else if (editingDestination) {
            destinationExpanded = false
            Qt.callLater(focusEntry)
        } else {
            Downloads.close()
        }
        return true
    }

    onDismissed: back()
    onInputActiveChanged: if (inputActive)
                              Qt.callLater(focusEntry)
    onRowsChanged: Qt.callLater(syncRows)
    onSelectingChanged: Qt.callLater(function () {
        syncRows()
        focusEntry()
    })
    onSelectedStateChanged: {
        const current = root.Window.window ? root.Window.window.activeFocusItem : null
        if (current === cancelButton || current === retryButton || current === playButton)
            Qt.callLater(focusJobAction)
    }
    Component.onCompleted: {
        syncRows()
        Qt.callLater(focusEntry)
    }

    AppText {
        Layout.fillWidth: true
        text: root.editingDestination ? "Download folder" : root.selecting ? "Download" : "Downloads"
        font.pixelSize: Metrics.titleSizePx
        font.weight: Font.DemiBold
    }

    SecondaryText {
        Layout.fillWidth: true
        text: root.editingDestination ? Platform.isWebOS
                                        ? "Choose a folder on a USB drive, such as /media/usb/Spool. Avoid the TV's own storage and check free space. Changes apply only to new downloads." :
                                          "Folder changes apply only to new downloads. Existing files stay where they are." :
                                          !Downloads.enabled
                                          ? "Downloads are off. Enable Allow downloads in Settings → Downloads to save new files. Existing files remain available offline." :
                                            root.selecting
                                            ? "Choose the version to save. Converted versions are prepared by your server." :
                                              "Keep Spool open to finish downloads. Downloaded files are available offline."
        wrapMode: Text.WordWrap
    }

    SecondaryText {
        Layout.fillWidth: true
        visible: !!Downloads.problem
        text: root.conciseError(Downloads.problem)
        color: Theme.errorText
        wrapMode: Text.WordWrap
        maximumLineCount: 2
        elide: Text.ElideRight
        Accessible.name: text
    }

    SecondaryText {
        Layout.fillWidth: true
        visible: !Downloads.mobile || root.destinationExpanded
        text: "Save new downloads to: " + Downloads.destination
        wrapMode: Text.WrapAnywhere
        maximumLineCount: 2
        elide: Text.ElideMiddle
        Accessible.name: "Save new downloads to " + Downloads.destination
    }

    SecondaryText {
        Layout.fillWidth: true
        visible: !root.editingDestination && root.selecting && root.options.length === 1
        text: visible ? String(root.options[0].label) : ""
        wrapMode: Text.WordWrap
    }

    ActionButton {
        id: downloadButton
        visible: !root.editingDestination && root.selecting && root.options.length === 1
        text: "Download"
        kind: "primary"
        onClicked: {
            Downloads.start(root.itemId, root.options[0])
            root.choosingAgain = false
        }
    }

    SecondaryText {
        Layout.fillWidth: true
        visible: Downloads.mobile && !Downloads.canChooseFolder
        text: "Stored in Spool on this device."
        wrapMode: Text.WordWrap
    }

    Flow {
        Layout.fillWidth: true
        visible: root.folderControls
        spacing: Metrics.scaled(8)
        ActionButton {
            id: chooseFolderButton
            text: "Choose folder…"
            iconName: "folder"
            onClicked: root.chooseDestination()
        }
        ActionButton {
            text: "Use default folder"
            kind: "flat"
            onClicked: Downloads.resetDestination()
        }
    }

    TextFieldRow {
        id: folderPath
        Layout.fillWidth: true
        visible: root.folderControls && root.destinationExpanded && Platform.isWebOS
        label: "Download folder path"
        placeholderText: "/media/usb/Spool"
        text: Downloads.destination
        inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoPredictiveText
        onAccepted: root.savePath()
    }

    ActionButton {
        visible: folderPath.visible
        text: "Use this folder"
        enabled: !!folderPath.text.trim()
        onClicked: root.savePath()
    }

    Repeater {
        model: root.folderControls && root.destinationExpanded && Downloads.mobile ? Downloads.destinationChoices : []
        delegate: ActionButton {
            required property var modelData
            Layout.fillWidth: true
            text: String(modelData.label)
            onClicked: Downloads.setDestination(modelData.url)
        }
    }

    SecondaryText {
        Layout.fillWidth: true
        visible: root.destinationExpanded && Downloads.mobile
        text: "Folder changes apply only to new downloads. App storage is removed when you uninstall Spool."
        wrapMode: Text.WordWrap
    }

    ListView {
        id: list
        objectName: "downloadsList"
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: root.folderBrowsing || (!root.editingDestination && (!root.selecting || root.options.length > 1))
        activeFocusOnTab: visible && count > 0
        clip: true
        focus: true
        keyNavigationEnabled: false
        model: downloadRows
        spacing: Metrics.scaled(8)
        currentIndex: 0
        onCurrentIndexChanged: {
            if (!root.reconcilingRows && !root.selecting && root.visibleJobs[currentIndex])
                root.selectedJobId = root.visibleJobs[currentIndex].id
        }
        delegate: MenuRow {
            id: downloadRow
            required property int index
            required property var record
            readonly property var modelData: record
            readonly property bool active: modelData.state === "preparing" || modelData.state === "downloading"
            readonly property bool unknown: active && (modelData.state === "preparing" || Number(modelData.total) <= 0)
            width: list.width
            rowHeight: Metrics.scaled(root.folderBrowsing ? 56 : root.selecting ? 76 : active ? 100 : 80)
            label: root.folderBrowsing || root.selecting ? String(modelData.label) : String(modelData.title)
            detail: root.folderBrowsing ? "" : root.selecting ? modelData.mode === "original" ? "Original file" :
                                                                                                "Converted by server" :
                                                                                                [root.statusText(
                                                                                                     modelData), String(
                                                                                                     modelData.quality
                                                                                                     || "")].filter(
                                                                                                    Boolean).join(" · ")
            iconName: root.folderBrowsing ? "folder" : root.selecting ? "download" : modelData.state === "complete"
                                                                        ? "download_done" : "download"
            highlighted: list.activeFocus && list.currentIndex === index
            Accessible.description: detail
            onHovered: list.currentIndex = index
            onActivated: {
                list.currentIndex = index
                InputKeys.focus(list)
                if (root.folderBrowsing) {
                    folderPath.text = modelData.path
                    list.currentIndex = 0
                    Qt.callLater(() => InputKeys.focus(list))
                } else if (root.selecting) {
                    Downloads.start(root.itemId, modelData)
                    root.choosingAgain = false
                } else {
                    root.selectedJobId = modelData.id
                    Qt.callLater(root.focusJobAction)
                }
            }
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: Metrics.scaled(12)
                height: Metrics.scaled(4)
                radius: height / 2
                color: Theme.border
                visible: downloadRow.active && !downloadRow.unknown
                Accessible.role: Accessible.ProgressBar
                Accessible.name: root.statusText(downloadRow.modelData)
                Accessible.description: "Download progress"
                Rectangle {
                    width: parent.width * Math.max(0, Math.min(1, Number(downloadRow.modelData.received) / Math.max(1, Number(
                                                                                                                        downloadRow.modelData.total))))
                    height: parent.height
                    radius: parent.radius
                    color: Theme.accent
                }
            }
            BusySpinner {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: Metrics.scaled(10)
                width: Metrics.scaled(18)
                height: width
                visible: downloadRow.unknown
                Accessible.role: Accessible.ProgressBar
                Accessible.name: root.statusText(downloadRow.modelData)
                running: visible && !Theme.reducedMotion
                color: Theme.textMuted
            }
        }
        ListScrollBar {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            flickable: list
        }
    }

    SecondaryText {
        Layout.fillWidth: true
        visible: !root.editingDestination && list.count === 0
        text: root.selecting ? "No download versions are available for this item." :
                               "Nothing downloaded yet. Use Download… in an item's menu."
        wrapMode: Text.WordWrap
    }

    SecondaryText {
        Layout.fillWidth: true
        visible: !root.editingDestination && !root.selecting && !!root.selectedJob.error
        text: root.conciseError(root.selectedJob.error)
        color: Theme.errorText
        wrapMode: Text.WordWrap
        maximumLineCount: 2
        elide: Text.ElideRight
        Accessible.name: text
    }

    Flow {
        Layout.fillWidth: true
        spacing: Metrics.scaled(8)
        ActionButton {
            id: cancelButton
            visible: !root.editingDestination && !root.selecting && root.activeJob
            text: "Cancel download"
            onClicked: {
                if (Number(root.selectedJob.received) > 0) {
                    root.cancelling = true
                    root.deletionJob = root.selectedJob
                } else {
                    Downloads.cancel(root.selectedJob.id)
                }
            }
        }
        ActionButton {
            id: retryButton
            visible: Downloads.enabled && !root.editingDestination && !root.selecting && (root.selectedJob.state
                                                                                          === "failed"
                                                                                          || root.selectedJob.state
                                                                                          === "cancelled")
            text: "Retry"
            onClicked: Downloads.retry(root.selectedJob.id)
        }
        ActionButton {
            visible: Downloads.enabled && !root.editingDestination && !root.selecting && !!root.itemId
                     && root.selectedJob.state === "failed"
            text: "Choose another version"
            onClicked: root.choosingAgain = true
        }
        ActionButton {
            id: playButton
            visible: !root.editingDestination && !root.selecting && root.selectedJob.state === "complete"
            text: "Play offline"
            iconName: "play_arrow"
            kind: "primary"
            onClicked: {
                const offlineId = Downloads.offlineItemId(root.selectedJob.id)
                if (offlineId) {
                    Downloads.close()
                    App.playItemId(offlineId)
                }
            }
        }
        ActionButton {
            id: removeButton
            visible: !root.editingDestination && !root.selecting && !!root.selectedJob.id && !root.activeJob
            text: root.selectedJob.state === "complete" ? "Delete downloaded file" : "Remove"
            kind: root.selectedJob.state === "complete" ? "danger" : "flat"
            onClicked: {
                if (root.selectedJob.state === "complete") {
                    root.cancelling = false
                    root.deletionJob = root.selectedJob
                } else {
                    Downloads.remove(root.selectedJob.id)
                    Qt.callLater(root.focusEntry)
                }
            }
        }
        ActionButton {
            id: closeButton
            text: root.editingDestination ? "Done" : "Close"
            kind: "flat"
            onClicked: {
                if (root.editingDestination) {
                    root.destinationExpanded = false
                    Qt.callLater(root.focusEntry)
                } else {
                    Downloads.close()
                }
            }
        }
    }

    Loader {
        id: folderDialog
        active: false
        source: active ? Qt.resolvedUrl("../pages/DesktopFolderDialog.qml") : ""
        onLoaded: {
            item.title = "Choose download folder"
            item.open()
        }
    }

    Connections {
        target: folderDialog.item
        function onFolderSelected(folder) {
            Downloads.setDestination(folder)
            root.focusEntry()
        }
        function onDismissed() {
            root.focusEntry()
        }
    }

    Loader {
        id: deletionDialog
        parent: root
        anchors.fill: parent
        z: 10
        active: root.deletionJob !== null
        sourceComponent: ConfirmationDialog {
            title: root.cancelling ? "Cancel download?" : "Delete downloaded file?"
            message: root.cancelling ? root.bytes(root.deletionJob ? root.deletionJob.received : 0)
                                       + " downloaded so far will be discarded." : "Delete “" + String(root.deletionJob
                                                                                                       ? root.deletionJob.title :
                                                                                                         "") + "” from this device? The server copy is not changed."
            confirmText: root.cancelling ? "Cancel download" : "Delete file"
            destructive: true
            onAccepted: {
                if (root.cancelling)
                    Downloads.cancel(root.deletionJob.id)
                else
                    Downloads.remove(root.deletionJob.id)
                root.deletionJob = null
                Qt.callLater(root.focusEntry)
            }
            onDismissed: root.back()
        }
    }
}
