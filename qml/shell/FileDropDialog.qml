import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Dialogs
import "../theme"
import "../primitives"

FocusScope {
    id: root
    required property var shell
    property bool blocked: false
    readonly property bool opened: phase !== ""
    property string phase: ""
    property var pendingDrops: []
    property var packages: []
    property var media: []
    property var preview: ({})
    property string errorMessage: ""
    property var returnFocus: null
    property string classificationRequestId: ""
    property string inspectionRequestId: ""
    property string installationToken: ""
    property string installationModuleId: ""
    readonly property bool installationProgressVisible: installationToken.length > 0 && Store.transfers.some(transfer
                                                                                                             => String(
                                                                                                                    transfer.operationToken
                                                                                                                    || "")
                                                                                                                === installationToken)
    readonly property var buttons: [cancelButton, inspectButton, playButton, appendButton, installButton,
        nextButton].filter(b => b.visible && b.enabled)
    anchors.fill: parent
    visible: opened && !blocked && (phase !== "installing" || !installationProgressVisible)

    function submitUrls(urls) {
        const ordered = []
        for (let i = 0; i < urls.length; ++i)
            ordered.push(urls[i])
        if (ordered.length === 0)
            return
        pendingDrops = pendingDrops.concat([ordered])
        drain()
    }
    function drain() {
        if (blocked || opened || files.visible || pendingDrops.length === 0)
            return
        const urls = pendingDrops[0]
        pendingDrops = pendingDrops.slice(1)
        packages = []
        media = []
        returnFocus = root.Window.window ? root.Window.window.activeFocusItem : null
        errorMessage = ""
        showPhase("classifying")
        classificationRequestId = Store.classifyFiles(urls)
    }
    function chooseClassifiedFiles() {
        if (packages.length > 0 && media.length > 0)
            showPhase("mixed")
        else if (packages.length > 0)
            inspectNext()
        else if (Player.sessionActive)
            showPhase("media")
        else {
            const urls = media.slice()
            finish()
            App.playLocalFiles(urls, false)
        }
    }
    function showPhase(value) {
        phase = value
        Qt.callLater(function () {
            if (root.visible)
                cancelButton.forceActiveFocus()
        })
    }
    function inspectNext() {
        media = []
        if (packages.length === 0) {
            finish()
            return
        }
        preview = ({})
        errorMessage = ""
        showPhase("inspecting")
        inspectionRequestId = Store.inspectFile(packages[0])
    }
    function finish() {
        const operationId = installationToken || String(preview.token || "") || inspectionRequestId
        if (operationId)
            Store.cancelInspection(operationId)
        phase = ""
        preview = ({})
        classificationRequestId = ""
        inspectionRequestId = ""
        installationToken = ""
        installationModuleId = ""
        packages = []
        media = []
        if (returnFocus && !blocked)
            InputKeys.focus(returnFocus)
        returnFocus = null
        Qt.callLater(drain)
    }
    function playFiles(append) {
        const urls = media.slice()
        finish()
        App.playLocalFiles(urls, append)
    }
    function install() {
        if (phase !== "consent" || !preview.token)
            return
        installationToken = String(preview.token)
        installationModuleId = String(preview.id)
        showPhase("installing")
        Store.installInspected(installationToken)
    }
    function openFiles() {
        if (!blocked && !opened)
            files.open()
    }
    function back() {
        if (phase !== "installing")
            finish()
        return true
    }
    function routeKey(key, inputPhase, repeat) {
        if (InputKeys.isBack(key, false, false)) {
            if (inputPhase === "release")
                back()
            return true
        }
        const selected = buttons.findIndex(b => b.activeFocus)
        if (InputKeys.isAccept(key)) {
            if (inputPhase === "press" && !repeat && selected < 0)
                cancelButton.forceActiveFocus()
            return true
        }
        if (InputKeys.isDirection(key) && inputPhase === "press") {
            if (selected < 0) {
                cancelButton.forceActiveFocus()
                return true
            }
            if (key === Qt.Key_Up || key === Qt.Key_Down)
                details.contentItem.contentY = Math.max(0, Math.min(details.contentItem.contentHeight - details.height,
                                                                    details.contentItem.contentY + (key === Qt.Key_Down
                                                                                                    ? 1 : -1)
                                                                    * Metrics.scaled(80)))
            else
                buttons[Math.max(0, Math.min(buttons.length - 1, selected + (key === Qt.Key_Right ? 1 :
                                                                                                    -1)))].forceActiveFocus(
                            )
        }
        return true
    }
    function activate() {
        // KeyRouter owns fresh-press/held-key suppression; opening releases never activate.
        const button = buttons.find(b => b.activeFocus)
        if (!button) {
            cancelButton.forceActiveFocus()
            return
        }
        button.clicked()
    }
    function describeList(label, values) {
        return label + ":\n" + (values && values.length ? values.map(v => "• " + String(v)).join("\n") :
                                                          "None declared") + "\n\n"
    }
    function packageDescription() {
        const p = preview
        return "Provider: " + String(p.name || "") + "\nID: " + String(p.id || "") + "\nPublisher (not verified): " + String(
                    p.publisher || "Not supplied") + "\nSource: local file — unverified executable provider code"
                + "\nInstalled source: " + String(p.installedProvenance || "Not installed") + "\nVersion: " + (
                    p.isUpdate ? String(p.installedVersion) + " → " : "") + String(p.version || "") + "\n\n" + String(
                    p.summary || "") + "\n\n" + describeList("Capabilities", p.capabilities) + describeList("Network origins",
                                                                                                            p.origins)
                + describeList("Added capabilities", p.addedCapabilities) + describeList("Removed capabilities",
                                                                                         p.removedCapabilities)
                + describeList("Added origins", p.addedOrigins) + describeList("Removed origins", p.removedOrigins)
                + "SHA-256 (byte integrity only, not publisher authentication):\n" + String(p.sha256 || "") + "\n\n"
                + String(p.warning
                         || "Installing a provider runs code that may access accounts and credentials you approve. Active accounts may restart. Install only if you trust this exact file.")
                + "\n\nInstallation approval does not approve sign-in, PIN access or network destinations."
    }
    onBlockedChanged: {
        if (!blocked) {
            Qt.callLater(function () {
                if (root.visible)
                    cancelButton.forceActiveFocus()
                else
                    root.drain()
            })
        }
    }
    Connections {
        target: Store
        function onFilesClassified(requestId, packages, media, error) {
            if (root.phase !== "classifying" || requestId !== root.classificationRequestId)
                return
            root.classificationRequestId = ""
            if (error) {
                root.errorMessage = String(error)
                root.showPhase("error")
                return
            }
            root.packages = packages
            root.media = media
            root.chooseClassifiedFiles()
        }
        function onFileInspectionFinished(requestId, preview, error) {
            if (root.phase !== "inspecting" || requestId !== root.inspectionRequestId)
                return
            root.inspectionRequestId = ""
            if (error) {
                root.errorMessage = String(error)
                root.showPhase("error")
                return
            }
            root.preview = preview
            root.showPhase("consent")
        }
        function onFileInstallationFinished(token, moduleId, error) {
            if (root.phase !== "installing" || token !== root.installationToken || (moduleId.length > 0 && moduleId
                                                                                    !== root.installationModuleId))
                return
            root.installationToken = ""
            root.installationModuleId = ""
            if (error) {
                root.errorMessage = String(error)
                root.showPhase("error")
                return
            }
            root.packages = root.packages.slice(1)
            root.inspectNext()
        }
    }
    FileDialog {
        id: files
        title: "Open files or Spool provider packages (.szo)"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["All files (*)"]
        onAccepted: root.submitUrls(selectedFiles)
        onVisibleChanged: if (!visible)
                              Qt.callLater(root.drain)
    }
    Rectangle {
        anchors.fill: parent
        color: "#99000000"
        MouseArea {
            anchors.fill: parent
        }
    }
    Surface {
        anchors.centerIn: parent
        width: Math.min(parent.width - 24, Metrics.scaled(760))
        height: Math.min(parent.height - 24, Metrics.scaled(700))
        elevated: true
        baseColor: Theme.floatingPanel
        PopupShield {}
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Math.min(Metrics.scaled(24), parent.width / 12)
            spacing: Metrics.scaled(12)
            AppText {
                Layout.fillWidth: true
                text: root.phase === "consent" ? (root.preview.isUpdate ? "Update provider?" : "Install provider?") :
                                                 root.phase === "mixed" ? "Providers and media were dropped together" :
                                                                          root.phase === "media" ? "Open local media" :
                                                                                                   root.phase
                                                                                                   === "error"
                                                                                                   ? "Could not install provider" :
                                                                                                     root.phase
                                                                                                     === "installing"
                                                                                                     ? "Installing provider…" :
                                                                                                       root.phase
                                                                                                       === "classifying"
                                                                                                       ? "Checking dropped files…" :
                                                                                                         "Inspecting provider…"
                textFormat: Text.PlainText
                font.pixelSize: Metrics.titleSizePx
                wrapMode: Text.Wrap
            }
            ScrollView {
                id: details
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth
                AppText {
                    width: details.availableWidth
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                    text: root.phase === "consent" ? root.packageDescription() : root.phase === "error"
                                                     ? root.errorMessage : root.phase === "mixed"
                                                       ? root.packages.length + " provider package(s), "
                                                         + root.media.length
                                                         + " media file(s). Choose one operation; nothing has been installed or played. Each provider is inspected and approved separately." :
                                                         root.phase === "media" ? "Play " + root.media.length
                                                                                  + " file(s) in their supplied order, or add them to the current local queue. Play now replaces the current playback. Playlist formats are handled by mpv. Mounted shares and UNC paths are preserved; availability and network protocol support depend on mpv."
                                                                                  + (Player.localPlaylist ? "" :
                                                                                                            "\n\nAdd to queue is available for local file sessions, not provider or group sessions.") :
                                                                                  root.phase === "inspecting"
                                                                                  ? "Reading the package without executing its JavaScript or QML. Cancel is the default." :
                                                                                    root.phase === "classifying"
                                                                                    ? "Checking package contents away from the interface thread. Nothing is installed or played until classification finishes." :
                                                                                      "Installing the exact approved package. See installation progress."
                    font.pixelSize: Metrics.bodySizePx
                }
            }
            Flow {
                Layout.fillWidth: true
                Layout.preferredHeight: childrenRect.height
                spacing: Metrics.scaled(8)
                ActionButton {
                    id: cancelButton
                    text: "Cancel"
                    enabled: root.phase !== "installing"
                    onClicked: root.finish()
                }
                ActionButton {
                    id: inspectButton
                    text: "Inspect providers"
                    visible: root.phase === "mixed"
                    onClicked: root.inspectNext()
                }
                ActionButton {
                    id: playButton
                    text: "Play now"
                    visible: root.phase === "mixed" || root.phase === "media"
                    onClicked: root.playFiles(false)
                }
                ActionButton {
                    id: appendButton
                    text: "Add to queue"
                    visible: root.phase === "mixed" || root.phase === "media"
                    enabled: Player.localPlaylist
                    onClicked: root.playFiles(true)
                }
                ActionButton {
                    id: installButton
                    text: root.preview.isUpdate ? "Trust and update" : "Trust and install"
                    visible: root.phase === "consent"
                    onClicked: root.install()
                }
                ActionButton {
                    id: nextButton
                    text: "Skip this package"
                    visible: root.phase === "error" && root.packages.length > 1
                    onClicked: {
                        root.packages = root.packages.slice(1)
                        root.inspectNext()
                    }
                }
            }
        }
    }
}
