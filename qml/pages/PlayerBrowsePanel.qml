import QtQuick
import Spool
import "../primitives"
import "../browse"
import "../theme"

// The sheet you add to the queue from, in the same right-edge slot the queue
// itself uses and a little wider, because it is showing artwork as well as
// text. Back returns to the queue rather than to the video, so adding several
// things is one gesture in and one gesture out.
FocusScope {
    id: root

    required property var overlay

    readonly property real panelWidth: {
        const available = parent ? parent.width : 0
        if (Metrics.lane(available) === "compact")
            return available
        return Math.min(Math.round(available * 0.52), Metrics.scaled(560))
    }

    signal dismissed

    // Both placements, on every row, rather than a mode to set first and
    // remember afterwards: the two are one press apart and each says where
    // the thing will land.
    readonly property var queueActions: [
        {
            "action": "next",
            "icon": "playlist_play",
            "label": "Play next"
        },
        {
            "action": "end",
            "icon": "playlist_add",
            "label": "Add to end"
        }
    ]

    function focusContent() {
        InputKeys.focus(surface)
        surface.focusList()
    }

    function routeKey(key, phase, repeat) {
        if (phase === "release")
            return true
        // Left steps out of a row's actions first; only once it has nothing
        // left to step out of does it close the sheet.
        if (key === Qt.Key_Left && !surface.routeKey(key, phase, repeat)) {
            root.dismissed()
            return true
        }
        if (key === Qt.Key_Left)
            return true
        return surface.routeKey(key, phase, repeat)
    }

    function activate() {
        surface.activate()
    }

    function back() {
        return surface.back()
    }

    function longPress() {
        return false
    }

    function finishOpeningGesture() {
    }

    // Queue a thing, or everything inside it. A season queued whole is the
    // difference between one press and twenty-two.
    function queue(item, action) {
        const next = action === "next"
        const type = String(item.itemType || "")
        if (type === "Series" || type === "Season") {
            App.queueEpisodicContainer(String(type === "Season" ? item.seriesId || "" : item.movieId || item.id || ""), type
                                       === "Season" ? String(item.movieId || item.id || "") : "", next)
            return
        }
        if (next)
            App.playNextFromItem(item)
        else
            App.addToQueueFromItem(item)
    }

    Surface {
        anchors.left: surface.left
        anchors.right: surface.right
        anchors.top: surface.top
        anchors.bottom: surface.bottom
        anchors.margins: -overlay.dp(14)
        baseColor: Theme.floatingPanel
        elevated: true
    }

    BrowseSurface {
        id: surface

        readonly property real inset: Metrics.pageMarginPx

        width: root.panelWidth
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.topMargin: inset
        anchors.bottomMargin: inset
        anchors.rightMargin: inset
        rootTitle: "Add to queue"
        rowActions: root.queueActions
        currentItemId: {
            const item = root.overlay.currentQueueItem
            return String((item && item.movieId) || "")
        }
        focus: true

        onDismissed: root.dismissed()
        onActionTriggered: (item, action) => root.queue(item, action)
    }
}
