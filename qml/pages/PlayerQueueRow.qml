import QtQuick
import QtQuick.Layouts
import Spool
import "../primitives"
import "../theme"

// One row of the queue outline. Most rows are a queue entry; some also carry
// the band for a folded run of episodes, and one carries the divider where
// what the queue filled in for itself gives way to what the user added.
//
// The band belongs to the run's first row rather than to a row of its own, so
// every visible row still maps to a real queue entry. When the run is open
// the band sits above that row's own content and its members are ruled
// together down the left edge -- shown as belonging to it without being
// indented out of line with everything else.
Item {
    id: row

    required property var overlay
    // The whole MovieItem. Art.url() reads every artwork tag off the gadget,
    // where a plain map would only carry the handful ArtworkService unpacks.
    required property var entry
    required property string entryType
    required property string title
    required property string subtitle
    required property string episodeCode
    required property bool genericEpisodeTitle
    required property bool playable
    required property real progress
    required property int position

    // Outline shape, from PlayQueueOutlineModel.
    property string outlineKind: "item"
    property string groupLabel: ""
    property string groupDetail: ""
    property int groupCount: 0
    property bool inGroup: false
    property bool userQueuedRunStart: false

    property bool current: false
    property bool highlighted: false
    property bool grabbed: false
    property bool reordering: false
    property bool pointerEnabled: false
    // A drag is passing over this row and will land here if released.
    property bool dropTarget: false

    readonly property bool folded: outlineKind === "group"
    readonly property bool unfolded: outlineKind === "groupOpen"
    readonly property bool hasBand: folded || unfolded
    // A folded row stands for its whole run, so there is no single entry to
    // play or remove and no duration that would mean anything.
    readonly property bool showsEntry: !folded

    // Hover-revealed controls, so a resting queue is just the list.
    readonly property bool pointerAffordances: pointerEnabled && rowHover.hovered

    signal activated
    signal removeRequested
    signal toggleRequested

    readonly property bool audio: entryType === "Audio" || entryType === "AudioBook"
    readonly property real artHeight: overlay.dp(audio ? 46 : 40)
    readonly property real artWidth: audio ? artHeight : Math.round(artHeight * 16 / 9)
    readonly property real bandHeight: overlay.dp(Platform.isTV ? 50 : 40)
    readonly property real entryHeight: overlay.dp(Platform.isTV ? 78 : 62)
    readonly property real dividerHeight: userQueuedRunStart ? overlay.dp(Platform.isTV ? 34 : 28) : 0

    implicitHeight: dividerHeight + (hasBand ? bandHeight : 0) + (showsEntry ? entryHeight : 0)

    function durationText() {
        const ticks = Number(entry && entry.runtimeTicks ? entry.runtimeTicks : 0)
        if (!(ticks > 0))
            return ""
        const total = Math.round(ticks / 10000000)
        const hours = Math.floor(total / 3600)
        const minutes = Math.floor((total % 3600) / 60)
        const seconds = total % 60
        const padded = seconds < 10 ? "0" + seconds : String(seconds)
        if (hours > 0)
            return hours + ":" + (minutes < 10 ? "0" + minutes : String(minutes)) + ":" + padded
        return minutes + ":" + padded
    }

    function primaryText() {
        if (entryType !== "Episode")
            return title
        // A bare "Episode 7" says nothing next to its own episode number, so
        // the code carries the row on its own in that case.
        return genericEpisodeTitle || episodeCode.length === 0 ? (episodeCode.length > 0 ? episodeCode : title) : episodeCode
                                                                 + " · " + title
    }

    function artworkKind() {
        return audio ? "square" : "landscape"
    }

    // Where the run that opened above this row is ruled, so its members read
    // as one block without being pushed out of line.
    Rectangle {
        visible: row.inGroup
        x: -overlay.dp(4)
        y: row.dividerHeight
        width: Math.max(1, overlay.dp(2))
        height: row.height - row.dividerHeight
        radius: width / 2
        color: Qt.alpha(Theme.accent, 0.45)
    }

    // The seam between what the queue filled in and what the user added.
    Item {
        id: divider

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: row.dividerHeight
        visible: row.userQueuedRunStart

        Rectangle {
            anchors.left: parent.left
            anchors.right: dividerLabel.left
            anchors.rightMargin: overlay.dp(10)
            anchors.verticalCenter: parent.verticalCenter
            height: 1
            color: Theme.borderStrong
        }

        SecondaryText {
            id: dividerLabel
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            text: "Added by you"
            color: Theme.textMuted
            font.pixelSize: Metrics.metaSizePx
        }

        Rectangle {
            anchors.left: dividerLabel.right
            anchors.leftMargin: overlay.dp(10)
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            height: 1
            color: Theme.borderStrong
        }
    }

    // The band for a folded run: how many rows it stands for, and where they
    // begin and end.
    Item {
        id: band

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: row.dividerHeight
        height: row.hasBand ? row.bandHeight : 0
        visible: row.hasBand

        Rectangle {
            anchors.fill: parent
            anchors.leftMargin: -overlay.dp(8)
            anchors.rightMargin: -overlay.dp(8)
            radius: Theme.radiusSmall
            color: row.grabbed ? Theme.focusedFill : (row.highlighted && !row.unfolded) ? Theme.bgHover : row.unfolded
                                                                                          ? Qt.alpha(Theme.accentPanel,
                                                                                                     0.35) : "transparent"
            border.width: row.grabbed || (row.highlighted && !row.unfolded) ? Theme.focusBorderWidth : 0
            border.color: row.grabbed ? Theme.accent : Qt.alpha(Theme.accent, 0.55)
        }

        RowLayout {
            anchors.fill: parent
            spacing: overlay.dp(12)

            Item {
                Layout.preferredWidth: overlay.dp(24)
                Layout.fillHeight: true

                MaterialIcon {
                    anchors.centerIn: parent
                    name: "chevron_right"
                    iconSize: overlay.dp(20)
                    iconColor: row.highlighted ? Theme.accent : Theme.textSecondary
                    rotation: row.unfolded ? 90 : 0

                    Behavior on rotation {
                        enabled: !Theme.reducedMotion
                        NumberAnimation {
                            duration: 120
                            easing.type: Easing.OutCubic
                        }
                    }
                }
            }

            AppText {
                Layout.fillWidth: true
                text: row.groupLabel
                color: row.highlighted ? Theme.textPrimary : Theme.textSecondary
                font.pixelSize: Metrics.bodySizePx
                font.weight: Font.DemiBold
                maximumLineCount: 1
                elide: Text.ElideRight
            }

            SecondaryText {
                visible: row.groupDetail.length > 0
                text: row.groupDetail
                color: Theme.textMuted
                font.pixelSize: Metrics.metaSizePx
                maximumLineCount: 1
                elide: Text.ElideRight
            }
        }

        TapHandler {
            enabled: row.pointerEnabled || !Platform.isTV
            onTapped: row.toggleRequested()
        }
    }

    // The queue entry itself.
    Item {
        id: entryBody

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: band.bottom
        anchors.bottom: parent.bottom
        visible: row.showsEntry

        Rectangle {
            anchors.fill: parent
            anchors.leftMargin: -overlay.dp(8)
            anchors.rightMargin: -overlay.dp(8)
            radius: Theme.radiusSmall
            color: row.grabbed ? Theme.focusedFill : row.highlighted ? Theme.bgHover : "transparent"
            border.width: row.grabbed || row.highlighted ? Theme.focusBorderWidth : 0
            border.color: row.grabbed ? Theme.accent : Qt.alpha(Theme.accent, 0.55)
        }

        // Where a dragged row would land if it were let go now.
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: -overlay.dp(8)
            anchors.rightMargin: -overlay.dp(8)
            height: Math.max(2, overlay.dp(2))
            radius: height / 2
            visible: row.dropTarget
            color: Theme.accent
        }

        RowLayout {
            anchors.fill: parent
            spacing: overlay.dp(12)

            // Position, or the state that replaces it: playing, or picked up.
            Item {
                Layout.preferredWidth: overlay.dp(24)
                Layout.fillHeight: true

                SecondaryText {
                    anchors.centerIn: parent
                    visible: !row.current && !row.grabbed
                    text: String(row.position)
                    color: Theme.textMuted
                    font.pixelSize: Metrics.metaSizePx
                }

                MaterialIcon {
                    anchors.centerIn: parent
                    visible: row.current && !row.grabbed
                    name: "graphic_eq"
                    iconSize: overlay.dp(18)
                    iconColor: Theme.accent
                }

                MaterialIcon {
                    anchors.centerIn: parent
                    visible: row.grabbed
                    name: "drag_indicator"
                    iconSize: overlay.dp(18)
                    iconColor: Theme.accent
                }
            }

            ImageCard {
                Layout.preferredWidth: row.artWidth
                Layout.preferredHeight: row.artHeight
                imageUrl: Art.url(row.entry, row.artworkKind(), Math.round(row.artWidth * 2))
                fallbackIcon: row.audio ? "music_note" : row.entryType === "Episode" ? "tv" : "movie"
                fallbackTint: Theme.bgHover

                // Resume position, on the artwork where the eye already is.
                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: overlay.dp(3)
                    visible: row.progress > 0
                    color: Qt.alpha("#000000", 0.55)

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: Math.round(parent.width * Math.min(1, Math.max(0, row.progress)))
                        color: Theme.accent
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0

                AppText {
                    Layout.fillWidth: true
                    text: row.primaryText()
                    color: row.playable ? (row.current ? Theme.accent : Theme.textPrimary) : Theme.textDisabled
                    font.pixelSize: Metrics.bodySizePx
                    font.weight: row.current ? Font.DemiBold : Font.Normal
                    maximumLineCount: 1
                    elide: Text.ElideRight
                }

                SecondaryText {
                    Layout.fillWidth: true
                    visible: row.subtitle.length > 0
                    text: row.subtitle
                    color: Theme.textSecondary
                    font.pixelSize: Metrics.metaSizePx
                    maximumLineCount: 1
                    elide: Text.ElideRight
                }
            }

            SecondaryText {
                visible: !removeButton.visible && text.length > 0
                text: row.durationText()
                color: Theme.textMuted
                font.pixelSize: Metrics.metaSizePx
            }

            // Pointer-only. A remote removes through the row's own menu, and an
            // extra focusable button would sit between every row and the next.
            IconButton {
                id: removeButton
                visible: row.pointerAffordances && !row.reordering
                focusPolicy: Qt.NoFocus
                chromeless: true
                iconName: "delete"
                accessibleName: "Remove from queue"
                onClicked: row.removeRequested()
            }
        }

        TapHandler {
            enabled: row.playable && !row.reordering
            onTapped: row.activated()
        }
    }

    HoverHandler {
        id: rowHover
        enabled: row.playable
    }
}
