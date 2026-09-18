pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../../theme"
import "../../primitives"

// A compact transport for the client this device is driving, kept in reach
// while the user browses everything else. Dragging its bar previews on the
// other screen exactly as the remote control page does, so the two are the
// same control in two places rather than two behaviours.
Item {
    id: root

    signal openRequested

    readonly property var item: RemoteControl.nowPlayingItem || ({})
    readonly property bool hasMedia: Boolean(item.movieId || item.title)
    readonly property bool shown: RemoteControl.targetSelected && (hasMedia || RemoteControl.playbackPending)
    property real shownPositionTicks: RemoteControl.positionTicks
    property string openMenu: ""

    // The frame at the position being shown, which during a drag is the one
    // under the finger. Falls back to the item's own artwork when the server
    // has no trickplay for it.
    readonly property var previewData: RemoteControl.trickplayAvailable ? RemoteControl.trickplayForTicks(
                                                                              shownPositionTicks) : ({})
    readonly property bool previewReady: previewData && previewData.available === true
    readonly property var audioTracks: RemoteControl.audioTracks || []
    readonly property var subtitleTracks: RemoteControl.subtitleTracks || []

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

    function toggleMenu(name) {
        openMenu = openMenu === name ? "" : name
    }

    implicitHeight: Metrics.scaled(87)
    visible: shown
    enabled: shown

    onShownChanged: if (!shown)
                        openMenu = ""

    Connections {
        target: RemoteControl
        function onStateChanged() {
            if (!seekBar.dragging)
                root.shownPositionTicks = RemoteControl.predictedPositionTicks()
        }
    }

    Timer {
        interval: 1000
        repeat: true
        running: root.shown && root.visible && !RemoteControl.paused
        onTriggered: if (!seekBar.dragging)
                         root.shownPositionTicks = RemoteControl.predictedPositionTicks()
    }

    // An enlarged frame while scrubbing, clear of the thumb doing the
    // scrubbing. The inline thumbnail tracks the same position, but at this
    // size the picture is actually readable.
    Item {
        width: Math.min(root.width - Metrics.scaled(24), Metrics.scaled(300))
        height: 0
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        visible: root.previewReady && seekBar.dragging
        z: 20

        Column {
            anchors.bottom: parent.top
            anchors.bottomMargin: Metrics.scaled(10)
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width
            spacing: Metrics.scaled(4)

            Rectangle {
                id: previewFrame
                readonly property real imageScale: root.previewReady && root.previewData.width > 0 ? width
                                                                                                     / root.previewData.width :
                                                                                                     1
                width: parent.width
                height: root.previewReady && root.previewData.width > 0 ? width * root.previewData.height
                                                                          / root.previewData.width : 0
                radius: Theme.radiusLarge
                color: "black"
                border.width: 1
                border.color: Theme.borderStrong
                clip: true

                Image {
                    source: root.previewReady ? "image://artwork/" + encodeURIComponent(root.previewData.url) : ""
                    x: root.previewReady ? root.previewData.offsetX * previewFrame.imageScale : 0
                    y: root.previewReady ? root.previewData.offsetY * previewFrame.imageScale : 0
                    width: root.previewReady ? root.previewData.sheetWidth * previewFrame.imageScale : 0
                    height: root.previewReady ? root.previewData.sheetHeight * previewFrame.imageScale : 0
                    fillMode: Image.Stretch
                    cache: true
                    asynchronous: true
                }
            }

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: previewClock.width + Metrics.scaled(14)
                height: previewClock.height + Metrics.scaled(6)
                radius: height / 2
                color: Theme.bgRaised

                AppText {
                    id: previewClock
                    anchors.centerIn: parent
                    text: root.formatTicks(root.shownPositionTicks)
                    color: Theme.textPrimary
                    font.pixelSize: Metrics.metaSizePx
                    font.weight: Font.DemiBold
                }
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bgRaised

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: Theme.borderStrong
        }

        // Opens the full remote. Sits behind the controls so their taps win.
        MouseArea {
            anchors.fill: parent
            onClicked: {
                if (root.openMenu.length > 0)
                    root.openMenu = ""
                else
                    root.openRequested()
            }
        }

        // Left: what is playing, and where.
        RowLayout {
            id: nowPlaying
            anchors.left: parent.left
            anchors.leftMargin: Metrics.scaled(12)
            anchors.right: transport.left
            anchors.rightMargin: Metrics.scaled(12)
            anchors.top: parent.top
            anchors.topMargin: Metrics.scaled(11)
            anchors.bottom: parent.bottom
            anchors.bottomMargin: Metrics.scaled(6)
            spacing: Metrics.scaled(11)
            // On a narrow screen the six transport buttons leave no room to
            // say what is playing. Losing the caption beats crushing it.
            visible: width > Metrics.scaled(96)

            Rectangle {
                id: thumb
                readonly property real imageScale: root.previewReady && root.previewData.width > 0 ? width
                                                                                                     / root.previewData.width :
                                                                                                     1
                Layout.preferredWidth: Metrics.scaled(78)
                Layout.preferredHeight: Metrics.scaled(44)
                Layout.alignment: Qt.AlignVCenter
                radius: Theme.radiusSmall
                color: "black"
                clip: true

                ImageCard {
                    anchors.fill: parent
                    visible: !root.previewReady
                    imageUrl: root.hasMedia ? Art.url(root.item, "landscape") : ""
                    fallbackText: ""
                    fallbackIcon: "cast_connected"
                }

                Image {
                    visible: root.previewReady
                    source: root.previewReady ? "image://artwork/" + encodeURIComponent(root.previewData.url) : ""
                    x: root.previewReady ? root.previewData.offsetX * thumb.imageScale : 0
                    y: root.previewReady ? root.previewData.offsetY * thumb.imageScale : 0
                    width: root.previewReady ? root.previewData.sheetWidth * thumb.imageScale : 0
                    height: root.previewReady ? root.previewData.sheetHeight * thumb.imageScale : 0
                    fillMode: Image.Stretch
                    cache: true
                    asynchronous: true
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Metrics.scaled(2)

                AppText {
                    Layout.fillWidth: true
                    text: RemoteControl.playbackPending ? "Starting " + (RemoteControl.pendingTitle || "playback") :
                                                          String(root.item.title || "Nothing playing")
                    color: Theme.textPrimary
                    font.pixelSize: Metrics.bodySizePx
                    font.weight: Font.DemiBold
                    maximumLineCount: 1
                    elide: Text.ElideRight
                }

                // The cast mark stands in for the word "on", so the row reads
                // as one phrase naming where this is playing.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Metrics.scaled(5)

                    MaterialIcon {
                        name: "cast_connected"
                        iconSize: Metrics.scaled(17)
                        iconColor: Theme.accent
                        Layout.preferredWidth: Metrics.scaled(17)
                        Layout.preferredHeight: Metrics.scaled(17)
                    }

                    SecondaryText {
                        Layout.fillWidth: true
                        text: RemoteControl.selectedTargetName
                        color: Theme.textMuted
                        font.pixelSize: Metrics.metaSizePx
                        maximumLineCount: 1
                        elide: Text.ElideRight
                    }

                    // Mirrors the group state so the controller and the client
                    // it drives both show they are in a SyncPlay group.
                    MaterialIcon {
                        visible: SyncPlay && SyncPlay.enabled
                        name: "groups"
                        iconSize: Metrics.scaled(17)
                        iconColor: Theme.accent
                        Layout.preferredWidth: Metrics.scaled(17)
                        Layout.preferredHeight: Metrics.scaled(17)
                    }
                }
            }
        }

        // Centre: the transport the player overlay offers, in the same order,
        // so the two read as one control in two places.
        RowLayout {
            id: transport
            readonly property real secondarySize: Math.max(Metrics.touchTargetPx * 0.82, Metrics.scaled(40))
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: Metrics.scaled(3)
            spacing: Metrics.scaled(2)

            IconButton {
                width: transport.secondarySize
                height: width
                iconRatio: 0.68
                iconName: "skip_previous"
                accessibleName: "Previous"
                onClicked: RemoteControl.previousTrack()
            }

            IconButton {
                width: transport.secondarySize
                height: width
                iconRatio: 0.68
                iconName: "replay_10"
                accessibleName: "Back 10 seconds"
                onClicked: RemoteControl.seekRelative(-100000000)
            }

            IconButton {
                width: Math.max(Metrics.touchTargetPx, Metrics.scaled(52))
                height: width
                iconRatio: 0.7
                iconName: RemoteControl.paused ? "play_arrow" : "pause"
                accessibleName: RemoteControl.paused ? "Play" : "Pause"
                selected: true
                onClicked: RemoteControl.togglePause()
            }

            IconButton {
                width: transport.secondarySize
                height: width
                iconRatio: 0.68
                iconName: "forward_10"
                accessibleName: "Forward 10 seconds"
                onClicked: RemoteControl.seekRelative(100000000)
            }

            IconButton {
                width: transport.secondarySize
                height: width
                iconRatio: 0.68
                iconName: "skip_next"
                accessibleName: "Next"
                onClicked: RemoteControl.nextTrack()
            }

            IconButton {
                width: transport.secondarySize
                height: width
                iconRatio: 0.68
                iconName: "stop"
                accessibleName: "Stop"
                onClicked: RemoteControl.stopPlayback()
            }
        }

        // Right: track selection, only for what this item actually offers.
        RowLayout {
            id: trackButtons
            anchors.right: parent.right
            anchors.rightMargin: Metrics.scaled(8)
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: Metrics.scaled(3)
            spacing: Metrics.scaled(2)

            IconButton {
                id: audioButton
                visible: root.audioTracks.length > 1
                iconRatio: 0.62
                iconName: "audiotrack"
                accessibleName: "Audio track"
                selected: root.openMenu === "audio"
                onClicked: root.toggleMenu("audio")
            }

            IconButton {
                id: subtitleButton
                visible: root.subtitleTracks.length > 0
                iconRatio: 0.62
                iconName: "subtitles"
                accessibleName: "Subtitles"
                selected: root.openMenu === "subtitles"
                onClicked: root.toggleMenu("subtitles")
            }
        }
    }

    // Both pickers open upward from their own button, so the button stays
    // under the thumb that opened it.
    Component {
        id: trackMenu

        Surface {
            id: panel
            property var tracks: []
            property string heading: ""
            property var chooser: null
            property Item anchorItem: null

            readonly property real panelWidth: Math.min(root.width - Metrics.scaled(16), Metrics.scaled(300))

            width: panelWidth
            height: Math.min(Metrics.scaled(300), column.implicitHeight + Metrics.scaled(12))
            x: anchorItem ? Math.max(Metrics.scaled(8), Math.min(root.width - width - Metrics.scaled(8),
                                                                 anchorItem.mapToItem(root, 0, 0).x + anchorItem.width
                                                                 / 2 - width / 2)) : 0
            y: -height - Metrics.scaled(8)
            z: 30
            elevated: true
            baseColor: Theme.bgRaised

            Flickable {
                anchors.fill: parent
                anchors.margins: Metrics.scaled(6)
                contentWidth: width
                contentHeight: column.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                clip: true

                Column {
                    id: column
                    width: parent.width

                    Repeater {
                        model: panel.tracks

                        delegate: MenuRow {
                            required property var modelData
                            width: column.width
                            compact: true
                            label: String(modelData.label || "Track")
                            checked: modelData.selected === true
                            onActivated: {
                                if (panel.chooser)
                                    panel.chooser(Number(modelData.index))
                                root.openMenu = ""
                            }
                        }
                    }
                }
            }
        }
    }

    Loader {
        active: root.openMenu === "audio"
        sourceComponent: trackMenu
        onLoaded: {
            item.tracks = Qt.binding(() => root.audioTracks)
            item.anchorItem = audioButton
            item.chooser = index => RemoteControl.selectAudioTrack(index)
        }
    }

    Loader {
        active: root.openMenu === "subtitles"
        sourceComponent: trackMenu
        onLoaded: {
            item.tracks = Qt.binding(() => root.subtitleTracks)
            item.anchorItem = subtitleButton
            item.chooser = index => RemoteControl.selectSubtitleTrack(index)
        }
    }

    // The seek bar rides the top edge, where it reads as the progress of the
    // bar itself rather than one more control competing with the row.
    InlineSlider {
        id: seekBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: -implicitHeight / 2 + Metrics.scaled(2)
        from: 0
        to: Math.max(1, RemoteControl.runtimeTicks)
        value: root.shownPositionTicks
        barHeight: Metrics.scaled(4)
        handleSize: dragging ? Metrics.scaled(18) : Metrics.scaled(11)
        interactionMargin: Metrics.scaled(18)
        accented: true
        enabled: root.hasMedia && RemoteControl.runtimeTicks > 0
        opacity: enabled ? 1 : 0
        z: 25

        onMoved: newValue => {
            root.shownPositionTicks = newValue
            if (dragging)
                RemoteControl.previewSeek(Math.round(newValue), true)
        }
        onCommitted: newValue => {
            RemoteControl.cancelSeekPreview()
            RemoteControl.seek(Math.round(newValue))
        }

        Behavior on handleSize {
            NumberAnimation {
                duration: 90
            }
        }
    }
}
