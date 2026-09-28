pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"
import "../shell"

FocusScope {
    id: root
    property bool compact: false
    readonly property real preferredMenuHeight: customContext ? Metrics.scaled(600) : Math.min(Metrics.scaled(680), Metrics.scaled(
                                                                                                   150) + list.contentHeight)
    property var customContext: null
    property var shell
    property var remote: RemoteTargets
    property var app: App
    property var localPlayer: Player
    readonly property bool contentReady: true
    property bool choosing: true
    property bool queueOpen: false
    property string focusedKey: compact ? "target:" : "chooser"
    property string editingKey: ""
    property real editedValue: 0
    readonly property var snapshot: remote.state || ({})
    readonly property var selected: remote.selectedTarget || ({})
    readonly property bool attached: remote.selectedTargetId.length > 0
    readonly property var commands: snapshot.commands || []
    // Position notifications update only the reading, not the row/queue model.
    readonly property bool seekKnown: remote.positionTicks !== "" && remote.runtimeTicks !== "" && Number(
                                          remote.runtimeTicks) > 0
    readonly property var rows: buildRows()
    readonly property int focusedIndex: rows.findIndex(row => row.key === focusedKey)
    property int lastFocusedIndex: 0
    onFocusedIndexChanged: if (focusedIndex >= 0)
                               lastFocusedIndex = focusedIndex
    readonly property var preview: snapshot.preview || null
    readonly property int previewFrame: preview ? Math.min(preview.count - 1, Math.max(0, Math.floor(editedValue * 1000
                                                                                                     / preview.intervalMs))) :
                                                  0
    readonly property int previewSheet: preview ? Math.floor(previewFrame / (preview.columns * preview.rows)) : 0
    readonly property int previewTile: preview ? previewFrame % (preview.columns * preview.rows) : 0

    function supports(action) {
        return attached && commands.indexOf(action) >= 0
    }
    function row(key, label, icon, extra) {
        return Object.assign({
                                 key: key,
                                 label: label,
                                 icon: icon || "",
                                 enabled: true
                             }, extra || {})
    }
    function buildRows() {
        const result = compact ? [] : [row("chooser", choosing ? "Hide device chooser" : "Choose playback device",
                                           "devices")]
        if (choosing) {
            // Local is always first, independent of progressive discovery order.
            result.push(row("target:", "This device", "tv", {
                                targetId: "",
                                checked: !attached
                            }))
            for (const target of remote.targets || []) {
                if (!target.id)
                    continue
                result.push(row("target:" + target.id, target.name, "cast", {
                                    targetId: target.id,
                                    detail: target.detail || "",
                                    checked: target.id === remote.selectedTargetId
                                }))
            }
            result.push(row("refresh", "Refresh devices", "refresh", {
                                enabled: !remote.busy
                            }))
        }
        if (!attached) {
            result.push(row("local", "Browse and play on this device", "home"))
            return result
        }
        if (localPlayer.sessionActive && supports("play"))
            result.push(row("transfer", "Transfer this device’s playback here", "cast", {
                                enabled: !remote.busy
                            }))
        const transport = [["previous", "Previous", "skip_previous"], ["pause", "Pause", "pause"], ["unpause", "Play",
                                                                                                    "play_arrow"],
                           ["stop", "Stop", "stop"], ["next", "Next", "skip_next"]]
        for (const item of transport) {
            if (!supports(item[0]))
                continue
            if (item[0] === "pause" && snapshot.state === "paused")
                continue
            if (item[0] === "unpause" && snapshot.state !== "paused" && snapshot.state !== "stopped")
                continue
            result.push(row(item[0], item[1], item[2], {
                                command: {
                                    action: item[0]
                                },
                                enabled: !remote.busy
                            }))
        }
        if (supports("seek") && seekKnown)
            result.push(row("seek", "Position", "schedule", {
                                adjustable: true
                            }))
        if (supports("volume") && snapshot.volume !== undefined)
            result.push(row("volume", "Volume", "volume_up", {
                                adjustable: true
                            }))
        if (supports("mute") && snapshot.muted !== undefined)
            result.push(row("mute", snapshot.muted ? "Unmute" : "Mute", "volume_off", {
                                command: {
                                    action: "mute",
                                    value: !snapshot.muted
                                }
                            }))
        if (supports("repeat")) {
            for (const mode of ["RepeatNone", "RepeatAll", "RepeatOne"])
                result.push(row("repeat:" + mode, mode === "RepeatNone" ? "Repeat off" : mode === "RepeatAll"
                                                                          ? "Repeat all" : "Repeat one", "repeat", {
                                    checked: snapshot.repeatMode === mode,
                                    command: {
                                        action: "repeat",
                                        mode: mode
                                    }
                                }))
        }
        if (supports("shuffle") && snapshot.shuffled !== undefined)
            result.push(row("shuffle", snapshot.shuffled ? "Disable shuffle" : "Enable shuffle", "shuffle", {
                                checked: snapshot.shuffled,
                                command: {
                                    action: "shuffle",
                                    value: !snapshot.shuffled
                                }
                            }))
        for (const kind of ["audio", "subtitle"]) {
            if (!supports(kind + "Track"))
                continue
            const tracks = snapshot[kind + "Tracks"] || []
            if (kind === "subtitle")
                result.push(row("subtitle:off", "Subtitles: Off", "subtitles", {
                                    checked: tracks.length > 0 && !tracks.some(track => track.selected),
                                    command: {
                                        action: "subtitleTrack",
                                        trackId: null
                                    }
                                }))
            for (const track of tracks)
                result.push(row(kind + ":" + track.id, (kind === "audio" ? "Audio: " : "Subtitles: ") + track.label,
                                kind === "audio" ? "audiotrack" : "subtitles", {
                                    checked: track.selected,
                                    command: {
                                        action: kind + "Track",
                                        trackId: track.id
                                    }
                                }))
        }
        result.push(row("queue", queueOpen ? "Hide queue" : "Show queue", "queue_music"))
        if (queueOpen) {
            const entries = remote.queue || []
            for (let i = 0; i < entries.length; ++i) {
                const entry = entries[i]
                const prefix = "entry:" + entry.entryId
                result.push(row(prefix, String(i + 1) + ". " + entry.title, "play_arrow", {
                                    entryId: entry.entryId,
                                    checked: snapshot.currentEntryId === entry.entryId,
                                    enabled: supports("queuePlay"),
                                    command: {
                                        action: "queuePlay",
                                        entryId: entry.entryId
                                    }
                                }))
                if (supports("queueRemove") && selected.queueEditing !== "none")
                    result.push(row(prefix + ":remove", "Remove “" + entry.title + "”", "remove_circle_outline", {
                                        entryId: entry.entryId,
                                        command: {
                                            action: "queueRemove",
                                            entryId: entry.entryId
                                        }
                                    }))
                if (supports("queueMove") && selected.queueEditing !== "none") {
                    if (i > 0)
                        result.push(row(prefix + ":up", "Move up", "arrow_upward", {
                                            entryId: entry.entryId,
                                            command: {
                                                action: "queueMove",
                                                entryId: entry.entryId,
                                                index: i - 1,
                                                afterEntryId: i > 1 ? entries[i - 2].entryId : null
                                            }
                                        }))
                    if (i + 1 < entries.length)
                        result.push(row(prefix + ":down", "Move down", "arrow_downward", {
                                            entryId: entry.entryId,
                                            command: {
                                                action: "queueMove",
                                                entryId: entry.entryId,
                                                index: i + 1,
                                                afterEntryId: entries[i + 1].entryId
                                            }
                                        }))
                    else if (remote.queueHasMore)
                        result.push(row(prefix + ":down", "Load next queue page to move down", "expand_more", {
                                            entryId: entry.entryId,
                                            loadAdjacent: true,
                                            enabled: !remote.queueBusy
                                        }))
                }
            }
            if (remote.queueHasMore)
                result.push(row("more", "Load more queue entries", "expand_more", {
                                    enabled: !remote.queueBusy
                                }))
            result.push(row("queueRefresh", "Refresh queue", "refresh", {
                                enabled: !remote.queueBusy
                            }))
        }
        if (selected.customControls)
            result.push(row("advanced", "Advanced device controls", "settings_remote"))
        result.push(row("disconnect", "Control this device instead", "tv"))
        return result
    }
    function syncVisibility() {
        remote.setChooserVisible(visible && choosing)
        remote.setQueueVisible(visible && attached && queueOpen)
    }
    onVisibleChanged: syncVisibility()
    onChoosingChanged: syncVisibility()
    onQueueOpenChanged: syncVisibility()
    onAttachedChanged: {
        editingKey = ""
        if (!attached) {
            choosing = true
            queueOpen = false
        }
        syncVisibility()
    }
    Connections {
        target: root.remote
        function onSelectionChanged() {
            root.editingKey = ""
            root.customContext = null
            root.syncVisibility()
        }
    }
    Component.onCompleted: syncVisibility()
    Component.onDestruction: {
        if (customContext)
            customContext.close()
        remote.setChooserVisible(false)
        remote.setQueueVisible(false)
    }
    onRowsChanged: {
        if (!rows.some(entry => entry.key === focusedKey)) {
            focusedKey = rows.length ? rows[Math.min(lastFocusedIndex, rows.length - 1)].key : ""
            editingKey = ""
        }
        Qt.callLater(revealFocusedRow)
    }
    function revealFocusedRow() {
        if (visible && Metrics.keyboardFocusActive && focusedIndex >= 0) {
            list.forceLayout()
            list.positionViewAtIndex(focusedIndex, ListView.Contain)
        }
    }
    function actionable(entry) {
        if (!entry || !entry.enabled)
            return false
        if (entry.entryId && remote.queueBusy)
            return false
        return !remote.busy || (!entry.command && !entry.adjustable && entry.key !== "transfer" && !entry.targetId)
    }
    function timeLabel(seconds) {
        if (!isFinite(seconds))
            return "Unknown"
        const whole = Math.max(0, Math.floor(seconds))
        return Math.floor(whole / 60) + ":" + (whole % 60 < 10 ? "0" : "") + whole % 60
    }
    function adjustmentValue(key) {
        return key === editingKey ? editedValue : key === "seek" ? Number(remote.positionTicks) / 10000000 : Number(
                                                                       snapshot.volume)
    }
    function stepperLabel(key) {
        return key === "seek" ? timeLabel(adjustmentValue(key)) + " / " + timeLabel(Number(remote.runtimeTicks)
                                                                                    / 10000000) : Math.round(
                                    adjustmentValue(key)) + "%"
    }
    function beginEditing(key) {
        if (editingKey === key)
            return
        editingKey = ""
        editedValue = adjustmentValue(key)
        editingKey = key
    }
    function adjust(key, direction) {
        beginEditing(key)
        const maximum = key === "seek" ? Math.floor(Number(remote.runtimeTicks) / 10000000) : 100
        editedValue = Math.max(0, Math.min(maximum, editedValue + direction * (key === "seek" ? 10 : 5)))
    }
    function commitEditing() {
        if (!editingKey)
            return
        const seconds = Math.floor(editedValue)
        const command = editingKey === "seek" ? {
                                                    action: "seek",
                                                    positionTicks: seconds === 0 ? "0" : String(seconds) + "0000000"
                                                } : {
            action: "volume",
            value: Math.round(editedValue)
        }
        editingKey = ""
        remote.send(command)
    }
    function perform(entry) {
        if (!actionable(entry))
            return
        focusedKey = entry.key
        if (entry.adjustable) {
            if (editingKey === entry.key)
                commitEditing()
            else
                beginEditing(entry.key)
        } else if (entry.targetId !== undefined) {
            remote.selectTarget(entry.targetId)
        } else if (entry.command)
            remote.send(entry.command)
        else if (entry.loadAdjacent)
            remote.requestQueuePage(false)
        else if (entry.key === "chooser")
            choosing = !choosing
        else if (entry.key === "refresh")
            remote.refreshTargets()
        else if (entry.key === "transfer")
            app.transferPlaybackToRemote()
        else if (entry.key === "queue")
            queueOpen = !queueOpen
        else if (entry.key === "more")
            remote.requestQueuePage(false)
        else if (entry.key === "queueRefresh")
            remote.requestQueuePage(true)
        else if (entry.key === "advanced") {
            if (compact && remote.createAdvancedControls)
                customContext = remote.createAdvancedControls()
            else
                remote.openAdvancedControls()
        } else if (entry.key === "disconnect" || entry.key === "local") {
            remote.disconnectTarget()
            if (entry.key === "local" && shell)
                shell.goHome()
        }
    }
    function activate() {
        if (customContext)
            customSurface.activate()
        else
            perform(rows[focusedIndex])
    }
    function back() {
        if (customContext) {
            customContext.close()
            customContext = null
            return true
        }
        if (editingKey) {
            editingKey = ""
            return true
        }
        if (queueOpen) {
            queueOpen = false
            focusedKey = "queue"
            return true
        }
        return false
    }
    function routeKey(key, phase, repeat) {
        if (customContext)
            return customSurface.routeKey(key, phase, repeat)
        if (!InputKeys.isDirection(key))
            return false
        if (phase === "release")
            return true
        if (InputKeys.isHorizontal(key)) {
            const entry = rows[focusedIndex]
            if (entry && entry.adjustable && editingKey === entry.key)
                adjust(entry.key, key === Qt.Key_Right ? 1 : -1)
            return true
        }
        if (editingKey)
            commitEditing()
        const next = focusedIndex + (key === Qt.Key_Down ? 1 : -1)
        if (next < 0 && shell)
            shell.focusNavBar()
        else if (rows.length) {
            focusedKey = rows[Math.max(0, Math.min(rows.length - 1, next))].key
            list.positionViewAtIndex(focusedIndex, ListView.Contain)
        }
        return true
    }

    Rectangle {
        anchors.fill: parent
        color: root.compact ? Theme.bgRaised : Theme.bg
        radius: root.compact ? Theme.radiusPanel : 0
        border.width: root.compact ? Theme.hoverBorderWidth : 0
        border.color: Theme.borderStrong
        MouseArea {
            anchors.fill: parent
            enabled: root.compact
            acceptedButtons: Qt.AllButtons
            onWheel: wheel => wheel.accepted = true
        }
    }
    ColumnLayout {
        visible: !root.customContext
        anchors.fill: parent
        anchors.margins: root.compact ? Metrics.scaled(16) : Metrics.pageMarginPx
        spacing: Metrics.gapPx
        RowLayout {
            Layout.fillWidth: true
            Image {
                visible: source.toString().length > 0
                source: root.snapshot.artwork || ""
                Layout.preferredWidth: Metrics.scaled(90)
                Layout.preferredHeight: Metrics.scaled(90)
                fillMode: Image.PreserveAspectFit
                asynchronous: true
            }
            ColumnLayout {
                Layout.fillWidth: true
                AppText {
                    Layout.fillWidth: true
                    text: root.attached ? root.selected.name || "Remote playback" : "Playback devices"
                    font.pixelSize: root.compact ? Metrics.bodySizePx + Metrics.scaled(2) : Metrics.titleSizePx
                    elide: Text.ElideRight
                }
                AppText {
                    Layout.fillWidth: true
                    text: root.attached ? (root.snapshot.title || (root.snapshot.state === "stopped" ? "Nothing playing" :
                                                                                                       "Remote media"))
                                          + " · " + (root.snapshot.state || "Connecting") :
                                          "Choosing a device does not start or transfer playback."
                    wrapMode: Text.WordWrap
                    font.pixelSize: Metrics.bodySizePx
                }
            }
        }
        AppText {
            Layout.fillWidth: true
            visible: !root.compact || root.remote.problem.length > 0 || root.remote.busy || root.editingKey.length > 0
            text: root.remote.problem || (root.remote.busy ? "Working…" : root.editingKey
                                                             ? "Left and Right adjust. OK saves; Back cancels." :
                                                               "Up and Down choose a control. OK activates or edits.")
            color: root.remote.problem ? Theme.errorText : Theme.textSecondary
            font.pixelSize: Metrics.bodySizePx
            wrapMode: Text.WordWrap
        }
        AppText {
            visible: root.queueOpen && root.selected.queueEditing === "replace"
            Layout.fillWidth: true
            text: "Queue changes restart remote playback. The current position is preserved when its entry remains."
            font.pixelSize: Metrics.bodySizePx
            color: Theme.textSecondary
            wrapMode: Text.WordWrap
        }
        Image {
            visible: root.editingKey === "seek" && root.preview !== null
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: Metrics.scaled(240)
            Layout.preferredHeight: root.preview ? width * root.preview.height / root.preview.width : 0
            source: visible ? root.preview.urlTemplate.replace("{index}", String(root.previewSheet)) : ""
            sourceClipRect: root.preview ? Qt.rect((root.previewTile % root.preview.columns) * root.preview.width,
                                                   Math.floor(root.previewTile / root.preview.columns)
                                                   * root.preview.height, root.preview.width, root.preview.height) :
                                           Qt.rect(0, 0, 0, 0)
            fillMode: Image.PreserveAspectFit
            asynchronous: true
        }
        ListView {
            id: list
            objectName: "remoteControlsList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.rows
            currentIndex: root.focusedIndex
            boundsBehavior: Flickable.StopAtBounds
            delegate: MenuRow {
                required property var modelData
                width: list.width
                label: modelData.label
                detail: modelData.detail || ""
                iconName: modelData.icon
                checked: Boolean(modelData.checked)
                highlighted: root.focusedKey === modelData.key
                actionable: root.actionable(modelData)
                stepperVisible: Boolean(modelData.adjustable)
                stepperEnabled: actionable
                stepperText: modelData.adjustable ? root.stepperLabel(modelData.key) : ""
                onActivated: {
                    root.perform(modelData)
                    InputKeys.focus(root)
                }
                onDecreaseRequested: {
                    root.adjust(modelData.key, -1)
                    root.commitEditing()
                }
                onIncreaseRequested: {
                    root.adjust(modelData.key, 1)
                    root.commitEditing()
                }
                Accessible.name: label
                Accessible.description: detail
                Accessible.selected: checked
            }
            FastWheelHandler {
                flickable: list
            }
        }
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Metrics.scaled(12)
        visible: root.customContext !== null
        ActionButton {
            text: "Back to devices"
            kind: "flat"
            iconName: "arrow_back"
            onClicked: root.back()
        }
        ProviderSurface {
            id: customSurface
            Layout.fillWidth: true
            Layout.fillHeight: true
            embedded: true
            context: root.customContext
            onFinished: {
                root.customContext = null
                InputKeys.focus(root)
            }
        }
    }
}
