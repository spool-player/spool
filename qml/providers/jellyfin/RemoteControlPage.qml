pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../../theme"
import "../../primitives"

FocusScope {
    id: root

    property var shell
    property real shownPositionTicks: RemoteControl.positionTicks
    readonly property bool contentReady: true
    readonly property var item: RemoteControl.nowPlayingItem || ({})
    readonly property bool hasMedia: Boolean(item.movieId || item.title)
    readonly property real remoteButtonSize: Math.max(Metrics.touchTargetPx, Math.min(Metrics.scaled(96), (width - 2
                                                                                                           * Metrics.pageMarginPx
                                                                                                           - Metrics.scaled(
                                                                                                               52)) / 3.17))
    readonly property real primaryRemoteButtonSize: Math.min(Metrics.scaled(112), remoteButtonSize * 1.17)
    readonly property var seekPreviewData: positionSlider.dragging && RemoteControl.trickplayAvailable
                                           ? RemoteControl.trickplayForTicks(shownPositionTicks) : ({})
    readonly property bool seekPreviewReady: seekPreviewData && seekPreviewData.available === true

    focus: true

    function formatTicks(ticks) {
        const total = Math.max(0, Math.floor(Number(ticks || 0) / 10000000))
        const hours = Math.floor(total / 3600)
        const minutes = Math.floor((total % 3600) / 60)
        const seconds = total % 60
        const minuteText = hours > 0 && minutes < 10 ? "0" + minutes : String(minutes)
        const secondText = seconds < 10 ? "0" + seconds : String(seconds)
        return hours > 0 ? hours + ":" + minuteText + ":" + secondText : minuteText + ":" + secondText
    }

    function selectedTrackLabel(tracks, fallback) {
        for (let index = 0; index < tracks.length; ++index)
            if (tracks[index].selected)
                return tracks[index].label
        return fallback
    }

    function cycleTrack(tracks, select) {
        if (!tracks || tracks.length === 0)
            return
        let selected = -1
        for (let index = 0; index < tracks.length; ++index)
            if (tracks[index].selected)
                selected = index
        const next = tracks[(selected + 1) % tracks.length]
        select(Number(next.index))
    }
    function openTargetKeyboard() {
        remoteKeyboard.text = ""
        InputKeys.focus(remoteKeyboard)
        Qt.inputMethod.show()
    }

    function commitTargetKeyboard() {
        const text = remoteKeyboard.text
        remoteKeyboard.text = ""
        if (text.length > 0)
            RemoteControl.sendGeneralCommand("SendString", {
                                                 "String": text
                                             })
        Qt.inputMethod.hide()
        Qt.callLater(function () {
            InputKeys.focus(keyboardButton)
        })
    }

    function routeKey(key, phase, repeat) {
        if (phase !== "press" || repeat || !InputKeys.isDirection(key))
            return InputKeys.isDirection(key)
        const window = root.Window.window
        const current = window ? window.activeFocusItem : null
        const backwards = key === Qt.Key_Left || key === Qt.Key_Up
        const next = current && current.nextItemInFocusChain ? current.nextItemInFocusChain(backwards) :
                                                               disconnectButton
        InputKeys.focus(next)
        if (next && next.mapToItem) {
            const point = next.mapToItem(content, 0, 0)
            flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, point.y - Metrics.scaled(80)))
        }
        return true
    }

    function activate() {
        const window = root.Window.window
        const current = window ? window.activeFocusItem : null
        if (current && current.clicked)
            current.clicked()
    }

    onActiveFocusChanged: if (activeFocus)
                              InputKeys.focus(RemoteControl.targetSelected ? disconnectButton : refreshButton)

    Connections {
        target: RemoteControl
        function onStateChanged() {
            if (!positionSlider.dragging)
                root.shownPositionTicks = RemoteControl.predictedPositionTicks()
        }
        function onTargetChanged() {
            if (RemoteControl.targetSelected)
                RemoteControl.refreshTargets()
        }
    }
    Connections {
        target: Qt.inputMethod
        function onVisibleChanged() {
            if (!Qt.inputMethod.visible && remoteKeyboard.activeFocus)
                root.commitTargetKeyboard()
        }
    }

    Timer {
        interval: 1000
        repeat: true
        running: root.visible && RemoteControl.targetSelected && !RemoteControl.paused
        onTriggered: if (!positionSlider.dragging)
                         root.shownPositionTicks = RemoteControl.predictedPositionTicks()
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.leftMargin: Metrics.pageMarginPx
        anchors.rightMargin: Metrics.pageMarginPx
        contentWidth: width
        contentHeight: content.implicitHeight + Metrics.pageMarginPx * 2
        boundsBehavior: Flickable.StopAtBounds
        flickDeceleration: Metrics.flickDecelerationPx
        maximumFlickVelocity: Metrics.maximumFlickVelocityPx
        clip: true

        ColumnLayout {
            id: content
            width: flick.width
            spacing: Metrics.scaled(18)

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Metrics.pageMarginPx
                spacing: Metrics.scaled(12)

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Metrics.scaled(2)

                    AppText {
                        Layout.fillWidth: true
                        text: RemoteControl.targetSelected ? RemoteControl.selectedTargetName : "Remote Control"
                        color: Theme.textPrimary
                        font.pixelSize: Metrics.titleSizePx
                        font.weight: Font.DemiBold
                    }

                    SecondaryText {
                        Layout.fillWidth: true
                        text: RemoteControl.targetSelected ? RemoteControl.selectedTargetDetail :
                                                             "Choose a Jellyfin client"
                        color: Theme.textMuted
                        font.pixelSize: Metrics.metaSizePx
                    }
                }

                ActionButton {
                    id: refreshButton
                    text: "Refresh"
                    iconName: "refresh"
                    onClicked: RemoteControl.refreshTargets()
                }

                ActionButton {
                    id: disconnectButton
                    visible: RemoteControl.targetSelected
                    text: "Disconnect"
                    iconName: "cast"
                    onClicked: RemoteControl.clearTarget()
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: !RemoteControl.targetSelected
                EmptyPlaceholder {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Metrics.scaled(120)
                    title: RemoteControl.busy ? "Looking for clients…" : "No client selected"
                    detail: RemoteControl.busy ? "Checking active Jellyfin sessions" :
                                                 "Use the Cast button or choose below"
                }

                Repeater {
                    model: RemoteControl.targets

                    delegate: ActionButton {
                        required property var modelData
                        Layout.fillWidth: true
                        text: (modelData.deviceName || modelData.client || "Jellyfin client") + (modelData.userName
                                                                                                 ? " — " + modelData.userName :
                                                                                                   "")
                        iconName: modelData.deviceType === "TV" ? "tv" : "devices"
                        onClicked: RemoteControl.selectTarget(modelData.sessionId)
                    }
                }
            }

            Surface {
                Layout.fillWidth: true
                Layout.preferredHeight: nowPlayingLayout.implicitHeight + Metrics.scaled(32)
                visible: RemoteControl.targetSelected
                elevated: true
                baseColor: Theme.bgRaised

                RowLayout {
                    id: nowPlayingLayout
                    anchors.fill: parent
                    anchors.margins: Metrics.scaled(16)
                    spacing: Metrics.scaled(16)

                    ImageCard {
                        Layout.preferredWidth: Metrics.scaled(root.hasMedia ? 128 : 84)
                        Layout.preferredHeight: width * 9 / 16
                        imageUrl: root.hasMedia ? Art.url(root.item, "landscape") : ""
                        fallbackText: root.hasMedia ? String(root.item.itemType || "Media") : "Idle"
                        fallbackIcon: root.hasMedia ? "movie" : "cast_connected"
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Metrics.scaled(4)

                        AppText {
                            Layout.fillWidth: true
                            text: RemoteControl.playbackPending ? "Starting " + (RemoteControl.pendingTitle
                                                                                 || "playback") : root.hasMedia ? String(
                                                                                                                      root.item.title
                                                                                                                      || "Playing") :
                                                                                                                  "Nothing playing"
                            color: Theme.textPrimary
                            font.pixelSize: Metrics.bodySizePx + Metrics.scaled(5)
                            font.weight: Font.DemiBold
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }

                        SecondaryText {
                            Layout.fillWidth: true
                            text: RemoteControl.playbackPending ? "Waiting for " + RemoteControl.selectedTargetName :
                                                                  String(root.item.seriesName || root.item.albumArtist
                                                                         || root.item.album || "")
                            font.pixelSize: Metrics.bodySizePx
                            visible: text.length > 0
                        }
                    }
                }
            }

            Surface {
                Layout.fillWidth: true
                Layout.preferredHeight: playbackColumn.implicitHeight + Metrics.scaled(32)
                visible: RemoteControl.targetSelected && root.hasMedia
                elevated: true
                baseColor: Theme.bgRaised

                ColumnLayout {
                    id: playbackColumn
                    anchors.fill: parent
                    anchors.margins: Metrics.scaled(16)
                    spacing: Metrics.scaled(12)

                    SectionHeader {
                        Layout.fillWidth: true
                        title: "Playback"
                    }

                    Item {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: Math.min(content.width - Metrics.scaled(16), Metrics.scaled(320))
                        Layout.preferredHeight: 0
                        visible: root.seekPreviewReady
                        z: 10

                        Column {
                            anchors.bottom: parent.top
                            anchors.bottomMargin: Metrics.scaled(8)
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: parent.width
                            spacing: Metrics.scaled(4)

                            Rectangle {
                                id: previewFrame
                                readonly property real imageScale: root.seekPreviewReady && root.seekPreviewData.width
                                                                   > 0 ? width / root.seekPreviewData.width : 1
                                width: parent.width
                                height: root.seekPreviewReady && root.seekPreviewData.width > 0 ? width
                                                                                                  * root.seekPreviewData.height
                                                                                                  / root.seekPreviewData.width :
                                                                                                  0
                                radius: Theme.radiusLarge
                                color: "black"
                                border.width: 1
                                border.color: Theme.borderStrong
                                clip: true

                                Image {
                                    source: root.seekPreviewReady ? "image://artwork/" + encodeURIComponent(
                                                                        root.seekPreviewData.url) : ""
                                    x: root.seekPreviewReady ? root.seekPreviewData.offsetX * previewFrame.imageScale :
                                                               0
                                    y: root.seekPreviewReady ? root.seekPreviewData.offsetY * previewFrame.imageScale :
                                                               0
                                    width: root.seekPreviewReady ? root.seekPreviewData.sheetWidth
                                                                   * previewFrame.imageScale : 0
                                    height: root.seekPreviewReady ? root.seekPreviewData.sheetHeight
                                                                    * previewFrame.imageScale : 0
                                    fillMode: Image.Stretch
                                    cache: true
                                    asynchronous: true
                                }
                            }

                            AppText {
                                id: previewClock
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: root.formatTicks(root.shownPositionTicks)
                                color: Theme.textPrimary
                                font.pixelSize: Metrics.bodySizePx
                                font.weight: Font.Medium
                            }
                        }
                    }

                    InlineSlider {
                        id: positionSlider
                        Layout.leftMargin: Metrics.scaled(8)
                        Layout.rightMargin: Metrics.scaled(8)
                        from: 0
                        to: Math.max(1, RemoteControl.runtimeTicks)
                        value: root.shownPositionTicks
                        interactionMargin: Metrics.scaled(16)
                        barHeight: Metrics.scaled(8)
                        handleSize: Metrics.scaled(22)
                        accented: true
                        onMoved: newValue => {
                            root.shownPositionTicks = newValue
                            if (dragging)
                                RemoteControl.previewSeek(Math.round(newValue), true)
                        }
                        onCommitted: newValue => {
                            RemoteControl.cancelSeekPreview()
                            RemoteControl.seek(Math.round(newValue))
                        }
                    }

                    RowLayout {

                        SecondaryText {
                            text: root.formatTicks(root.shownPositionTicks)
                        }
                        Item {}
                        SecondaryText {
                            text: root.formatTicks(RemoteControl.runtimeTicks)
                        }
                    }

                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: Metrics.scaled(8)

                        IconButton {
                            iconName: "skip_previous"
                            accessibleName: "Previous"
                            onClicked: RemoteControl.previousTrack()
                        }
                        IconButton {
                            iconName: "replay_10"
                            accessibleName: "Back 10 seconds"
                            onClicked: RemoteControl.seekRelative(-100000000)
                        }
                        IconButton {
                            width: Math.max(Metrics.touchTargetPx, Metrics.scaled(62))
                            height: width
                            iconName: RemoteControl.paused ? "play_arrow" : "pause"
                            accessibleName: RemoteControl.paused ? "Play" : "Pause"
                            selected: true
                            onClicked: RemoteControl.togglePause()
                        }
                        IconButton {
                            iconName: "forward_10"
                            accessibleName: "Forward 10 seconds"
                            onClicked: RemoteControl.seekRelative(100000000)
                        }
                        IconButton {
                            iconName: "skip_next"
                            accessibleName: "Next"
                            onClicked: RemoteControl.nextTrack()
                        }
                        IconButton {
                            iconName: "stop"
                            accessibleName: "Stop"
                            onClicked: RemoteControl.stopPlayback()
                        }
                    }

                    RowLayout {
                        spacing: Metrics.scaled(12)

                        IconButton {
                            iconName: RemoteControl.muted ? "volume_off" : "volume_up"
                            accessibleName: RemoteControl.muted ? "Unmute" : "Mute"
                            onClicked: RemoteControl.toggleMute()
                        }

                        InlineSlider {
                            id: volumeSlider
                            from: 0
                            to: 100
                            value: RemoteControl.volume
                            stepSize: 1
                            interactionMargin: Metrics.scaled(16)
                            onCommitted: newValue => RemoteControl.setVolume(Math.round(newValue))
                        }

                        SecondaryText {
                            text: RemoteControl.volume + "%"
                            color: Theme.textSecondary
                        }
                    }

                    RowLayout {
                        spacing: Metrics.scaled(8)

                        ActionButton {
                            visible: RemoteControl.audioTracks.length > 1
                            text: "Audio: " + root.selectedTrackLabel(RemoteControl.audioTracks, "Default")
                            iconName: "audiotrack"
                            onClicked: root.cycleTrack(RemoteControl.audioTracks, function (index) {
                                RemoteControl.selectAudioTrack(index)
                            })
                        }
                        ActionButton {
                            visible: RemoteControl.subtitleTracks.length > 0
                            text: "Subtitles: " + root.selectedTrackLabel(RemoteControl.subtitleTracks, "Off")
                            iconName: "subtitles"
                            onClicked: root.cycleTrack(RemoteControl.subtitleTracks, function (index) {
                                RemoteControl.selectSubtitleTrack(index)
                            })
                        }
                        ActionButton {
                            text: RemoteControl.repeatMode === "RepeatNone" ? "Repeat off" : RemoteControl.repeatMode
                            iconName: "repeat"
                            onClicked: RemoteControl.setRepeatMode(RemoteControl.repeatMode === "RepeatNone"
                                                                   ? "RepeatAll" : RemoteControl.repeatMode
                                                                     === "RepeatAll" ? "RepeatOne" : "RepeatNone")
                        }
                        ActionButton {
                            text: RemoteControl.shuffled ? "Shuffled" : "Shuffle"
                            iconName: "shuffle"
                            onClicked: RemoteControl.setShuffled(!RemoteControl.shuffled)
                        }
                    }
                }
            }

            Surface {
                Layout.fillWidth: true
                Layout.preferredHeight: navigationColumn.implicitHeight + Metrics.scaled(32)
                visible: RemoteControl.targetSelected
                elevated: true
                baseColor: Theme.bgRaised

                ColumnLayout {
                    id: navigationColumn
                    anchors.fill: parent
                    anchors.margins: Metrics.scaled(16)
                    spacing: Metrics.scaled(14)

                    SectionHeader {
                        Layout.fillWidth: true
                        title: "Navigation"
                    }

                    SecondaryText {
                        Layout.fillWidth: true
                        text: "Control the screen on " + RemoteControl.selectedTargetName
                    }

                    GridLayout {
                        Layout.alignment: Qt.AlignHCenter
                        columns: 3
                        rowSpacing: Metrics.scaled(10)
                        columnSpacing: Metrics.scaled(10)

                        Item {
                            Layout.preferredWidth: root.remoteButtonSize
                            Layout.preferredHeight: root.remoteButtonSize
                        }
                        IconButton {
                            width: root.remoteButtonSize
                            height: width
                            iconName: "keyboard_arrow_up"
                            accessibleName: "Up"
                            onClicked: RemoteControl.sendGeneralCommand("MoveUp")
                        }
                        Item {
                            Layout.preferredWidth: root.remoteButtonSize
                            Layout.preferredHeight: root.remoteButtonSize
                        }
                        IconButton {
                            width: root.remoteButtonSize
                            height: width
                            iconName: "keyboard_arrow_left"
                            accessibleName: "Left"
                            onClicked: RemoteControl.sendGeneralCommand("MoveLeft")
                        }
                        IconButton {
                            width: root.primaryRemoteButtonSize
                            height: width
                            iconName: "radio_button_checked"
                            accessibleName: "Select"
                            selected: true
                            onClicked: RemoteControl.sendGeneralCommand("Select")
                        }
                        IconButton {
                            width: root.remoteButtonSize
                            height: width
                            iconName: "keyboard_arrow_right"
                            accessibleName: "Right"
                            onClicked: RemoteControl.sendGeneralCommand("MoveRight")
                        }
                        IconButton {
                            width: root.remoteButtonSize
                            height: width
                            iconName: "arrow_back"
                            accessibleName: "Back"
                            onClicked: RemoteControl.sendGeneralCommand("Back")
                        }
                        IconButton {
                            width: root.remoteButtonSize
                            height: width
                            iconName: "keyboard_arrow_down"
                            accessibleName: "Down"
                            onClicked: RemoteControl.sendGeneralCommand("MoveDown")
                        }
                        IconButton {
                            width: root.remoteButtonSize
                            height: width
                            iconName: "menu"
                            accessibleName: "Menu"
                            onClicked: RemoteControl.sendGeneralCommand("ToggleContextMenu")
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Metrics.scaled(8)

                        ActionButton {
                            Layout.fillWidth: true
                            text: "Home"
                            iconName: "home"
                            onClicked: RemoteControl.sendGeneralCommand("GoHome")
                        }
                        ActionButton {
                            Layout.fillWidth: true
                            text: "Search"
                            iconName: "search"
                            onClicked: RemoteControl.sendGeneralCommand("GoToSearch")
                        }
                        ActionButton {
                            id: keyboardButton
                            Layout.fillWidth: true
                            visible: !Platform.isTV
                            text: "Keyboard"
                            iconName: "keyboard"
                            onClicked: root.openTargetKeyboard()
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Metrics.scaled(8)

                        ActionButton {
                            Layout.fillWidth: true
                            text: "Settings"
                            iconName: "settings"
                            onClicked: RemoteControl.sendGeneralCommand("GoToSettings")
                        }
                        ActionButton {
                            Layout.fillWidth: true
                            text: "Player overlay"
                            iconName: "picture_in_picture_alt"
                            onClicked: RemoteControl.sendGeneralCommand("ToggleOsd")
                        }
                    }

                    TextInput {
                        id: remoteKeyboard
                        Layout.preferredWidth: 1
                        Layout.preferredHeight: 1
                        opacity: 0
                        inputMethodHints: Qt.ImhNoPredictiveText
                        onAccepted: root.commitTargetKeyboard()
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: RemoteControl.targetSelected && RemoteControl.queue.length > 0
                spacing: Metrics.scaled(6)

                SectionHeader {
                    Layout.fillWidth: true
                    title: "Playback queue"
                }

                Repeater {
                    model: RemoteControl.queue

                    delegate: Surface {
                        required property int index
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredHeight: Metrics.controlHeightPx + Metrics.scaled(10)
                        baseColor: modelData.current ? Theme.accentPanel : Theme.bgPanel

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Metrics.scaled(14)
                            anchors.rightMargin: Metrics.scaled(8)
                            spacing: Metrics.scaled(6)

                            AppText {
                                Layout.fillWidth: true
                                text: (modelData.current ? "Now playing · " : "") + (modelData.title
                                                                                     || modelData.seriesName
                                                                                     || "Queue item " + (index + 1))
                                color: Theme.textPrimary
                                font.pixelSize: Metrics.bodySizePx
                                elide: Text.ElideRight
                            }
                            IconButton {
                                iconName: "play_arrow"
                                accessibleName: "Play queue item"
                                onClicked: RemoteControl.playQueueItem(index)
                            }
                            IconButton {
                                enabled: index > 0
                                iconName: "keyboard_arrow_up"
                                accessibleName: "Move up"
                                onClicked: RemoteControl.moveQueueItem(index, index - 1)
                            }
                            IconButton {
                                enabled: index + 1 < RemoteControl.queue.length
                                iconName: "keyboard_arrow_down"
                                accessibleName: "Move down"
                                onClicked: RemoteControl.moveQueueItem(index, index + 1)
                            }
                            IconButton {
                                iconName: "delete"
                                accessibleName: "Remove"
                                onClicked: RemoteControl.removeQueueItem(index)
                            }
                        }
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: Metrics.pageMarginPx
            }
        }
    }
}
