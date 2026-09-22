import QtQuick
import Spool
import "../primitives"
import "../theme"

// The queue as a right-edge sheet over live video, replacing the dropdown that
// borrowed the playback-settings menu. Built on the SubtitleSettingsPanel
// shape: a Surface anchored to the list rather than the parent, so the panel
// hugs the edge and leaves the picture it is queueing against on screen.
//
// The list shows PlayQueue.outline rather than the queue itself: runs the
// queue filled in for itself are folded, so starting an episode of a long
// series no longer opens onto several hundred rows. Rows here are outline
// rows, and the two index spaces are kept apart on purpose -- everything that
// edits the queue speaks in source rows, and everything that moves focus
// speaks in outline rows, with sourceRowAt/rowForSourceRow between them.
FocusScope {
    id: root

    required property var overlay

    readonly property var queue: PlayQueue
    readonly property var outline: PlayQueue.outline
    // Editing works in a SyncPlay group too: AppController publishes the change
    // to the server and the group's broadcast settles everyone's queue.
    readonly property bool editable: true

    // A picked-up block, whether by remote or by pointer. Both funnel through
    // the same preview/commit pair so there is one reorder path to reason
    // about, and both speak in source rows because that is what the queue
    // itself understands. A single row is a block of one.
    property int grabbedRow: -1
    property int grabbedCount: 0
    property int grabbedOrigin: -1
    property bool pointerDragging: false
    readonly property bool reordering: grabbedRow >= 0

    signal dismissed
    signal addRequested

    // A sheet takes a share of the picture it is queueing against, capped so
    // a wide window does not hand it half the film. A window narrow enough
    // that a share of it would not hold a row of episode text gets the sheet
    // over the whole screen instead, which is the phone form of the same rule.
    readonly property real panelWidth: {
        const available = parent ? parent.width : 0
        if (Metrics.lane(available) === "compact")
            return available
        return Math.min(Math.round(available * 0.42), Metrics.scaled(430))
    }

    function focusSourceRow(sourceRow) {
        const target = root.outline.rowForSourceRow(sourceRow)
        list.currentIndex = Math.max(0, Math.min(target < 0 ? 0 : target, list.count - 1))
        InputKeys.focus(list)
    }

    // Kept for the overlay's existing call, which knows only about the queue.
    function focusRow(index) {
        focusSourceRow(index)
    }

    function spanAt(row) {
        return root.outline.groupSpanAt(row)
    }

    function beginGrab(row) {
        const span = spanAt(row)
        if (Number(span.count) <= 0)
            return false
        root.grabbedRow = Number(span.first)
        root.grabbedCount = Number(span.count)
        root.grabbedOrigin = Number(span.first)
        return true
    }

    function endGrab() {
        root.grabbedRow = -1
        root.grabbedCount = 0
        root.grabbedOrigin = -1
    }

    function previewMoveTo(sourceRow) {
        if (grabbedRow < 0 || sourceRow === grabbedRow)
            return false
        if (!App.previewQueueMoveRange(grabbedRow, grabbedCount, sourceRow))
            return false
        grabbedRow = sourceRow
        return true
    }

    function cancelReorder() {
        if (grabbedRow < 0)
            return false
        if (grabbedRow !== grabbedOrigin)
            App.previewQueueMoveRange(grabbedRow, grabbedCount, grabbedOrigin)
        const origin = grabbedOrigin
        endGrab()
        pointerDragging = false
        list.interactive = true
        focusSourceRow(origin)
        return true
    }

    function dropReorder() {
        if (grabbedRow < 0)
            return false
        App.commitQueueMoveRange(grabbedOrigin, grabbedCount, grabbedRow)
        const landed = grabbedRow
        endGrab()
        pointerDragging = false
        list.interactive = true
        focusSourceRow(landed)
        return true
    }

    // Where a block would land if it were let go over this outline row. The
    // block is lifted out first, so a row below it has already shifted up by
    // the block's own size -- which is why the two directions differ.
    function dropTargetFor(row) {
        const span = spanAt(row)
        const first = Number(span.first)
        const count = Number(span.count)
        if (first < 0 || count <= 0 || first === grabbedRow)
            return -1
        if (first > grabbedRow)
            return first + count - grabbedCount
        return first
    }

    function dragToScenePosition(sceneY) {
        if (grabbedRow < 0)
            return
        const local = list.mapFromItem(null, 0, sceneY)
        const row = list.indexAt(list.width / 2, local.y + list.contentY)
        if (row < 0)
            return
        const target = dropTargetFor(row)
        if (target >= 0)
            previewMoveTo(target)
    }

    function stepGrabbed(delta) {
        const at = root.outline.rowForSourceRow(grabbedRow)
        const neighbour = at + delta
        if (at < 0 || neighbour < 0 || neighbour >= list.count)
            return true
        const target = dropTargetFor(neighbour)
        if (target >= 0 && previewMoveTo(target)) {
            const moved = root.outline.rowForSourceRow(grabbedRow)
            list.currentIndex = moved
            list.positionViewAtIndex(moved, ListView.Contain)
        }
        return true
    }

    // KeyRouter defers activation to key release for any target that owns a
    // longPress member, so VideoSurface only advertises one while this panel is
    // open. Returning false here leaves the press behaving normally.
    function longPress() {
        if (!editable || grabbedRow >= 0 || list.count < 2)
            return false
        if (list.currentIndex < 0 || list.currentIndex >= list.count)
            return false
        return beginGrab(list.currentIndex)
    }

    // Reached on the release that completed the grab gesture. Dropping here
    // would undo the pick-up the user just made.
    function finishOpeningGesture() {
    }

    function toggleCurrentGroup() {
        if (list.currentIndex < 0)
            return false
        const sourceRow = root.outline.sourceRowAt(list.currentIndex)
        if (!root.outline.toggleGroup(list.currentIndex))
            return false
        // The row that drew the band is still the row it was; keep the cursor
        // on it so opening a run does not throw focus somewhere else.
        focusSourceRow(sourceRow)
        return true
    }

    function activate() {
        if (grabbedRow >= 0) {
            dropReorder()
            return
        }
        if (list.currentIndex < 0 || list.currentIndex >= list.count)
            return
        // A folded row stands for a whole run, so there is nothing single to
        // play: opening it is the only thing activation can sensibly mean.
        if (list.currentItem && list.currentItem.folded) {
            toggleCurrentGroup()
            return
        }
        App.playQueueItem(root.outline.sourceRowAt(list.currentIndex))
    }

    function back() {
        if (cancelReorder())
            return true
        root.dismissed()
        return true
    }

    function routeKey(key, phase, repeat) {
        if (phase === "release")
            return true

        if (grabbedRow >= 0) {
            if (key === Qt.Key_Up)
                return stepGrabbed(-1)
            if (key === Qt.Key_Down)
                return stepGrabbed(1)
            // Nothing else should escape a grab: leaving the panel mid-move
            // would strand the row somewhere the user did not choose.
            return true
        }

        // Left returns to the video and its transport; Right opens or shuts
        // the run at the cursor, which is the one thing to its right.
        if (key === Qt.Key_Left) {
            root.dismissed()
            return true
        }
        if (key === Qt.Key_Right) {
            toggleCurrentGroup()
            return true
        }

        if (key === Qt.Key_Down && list.currentIndex >= list.count - 1) {
            addButton.forceActiveFocus()
            return true
        }
        return list.routeKey(key, phase, repeat)
    }

    Surface {
        anchors.left: list.left
        anchors.right: list.right
        anchors.top: heading.top
        anchors.bottom: addButton.bottom
        anchors.margins: -overlay.dp(14)
        baseColor: Theme.floatingPanel
        elevated: true
    }

    Item {
        id: heading

        anchors.top: parent.top
        anchors.left: list.left
        anchors.right: list.right
        anchors.topMargin: list.inset
        height: Math.max(closeButton.height, headingText.implicitHeight + hint.height)

        AppText {
            id: headingText
            anchors.left: parent.left
            anchors.right: closeButton.visible ? closeButton.left : parent.right
            anchors.rightMargin: Metrics.scaled(12)
            anchors.top: parent.top
            text: "Queue"
            font.pixelSize: Metrics.titleSizePx
            font.weight: Font.DemiBold
            maximumLineCount: 1
            elide: Text.ElideRight
        }

        // A picked-up row is the one state with no obvious way out, so it says
        // so rather than leaving the user to guess at the remote.
        SecondaryText {
            id: hint
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: headingText.bottom
            height: visible ? implicitHeight : 0
            visible: root.reordering || (root.overlay.syncPlayActive && list.count > 0)
            text: root.reordering ? "Moving — Up/Down to place, OK to drop, Back to cancel" :
                                    "Changes apply to everyone in the group"
            color: root.reordering ? Theme.accent : Theme.textMuted
            font.pixelSize: Metrics.metaSizePx
            maximumLineCount: 1
            elide: Text.ElideRight
        }

        // Pointer-only, matching the subtitle panel: a remote closes with Back,
        // and a focusable button here is just one more stop before the rows.
        IconButton {
            id: closeButton
            visible: !Platform.isTV
            anchors.right: parent.right
            anchors.top: parent.top
            focusPolicy: Qt.NoFocus
            chromeless: true
            iconName: "close"
            accessibleName: "Close queue"
            onClicked: root.dismissed()
        }
    }

    MenuListView {
        id: list

        readonly property real inset: Metrics.pageMarginPx

        width: root.panelWidth
        anchors.top: heading.bottom
        anchors.bottom: addButton.top
        anchors.right: parent.right
        anchors.topMargin: Metrics.scaled(12)
        anchors.bottomMargin: Metrics.scaled(10)
        anchors.rightMargin: inset
        spacing: overlay.dp(4)

        // The panel owns Back and the horizontals; the list only walks rows.
        dismissOnBack: false
        dismissOnHorizontal: false

        model: root.outline

        // The drag lives here rather than on a row, because a row can be
        // destroyed mid-drag: previewing a move re-folds the outline around
        // where the block now sits, which can take whole runs in and out of
        // view. A handler on the list outlives all of that; the row under the
        // press is found by hit testing instead of by owning the handler.
        DragHandler {
            id: listDrag
            enabled: !Platform.isTV && root.editable && list.count > 1
            target: null
            xAxis.enabled: false
            yAxis.enabled: true

            onActiveChanged: {
                if (active) {
                    const row = list.indexAt(list.width / 2, centroid.pressPosition.y + list.contentY)
                    if (row < 0 || !root.beginGrab(row)) {
                        return
                    }
                    root.pointerDragging = true
                    list.interactive = false
                    list.currentIndex = row
                    return
                }
                if (root.pointerDragging)
                    root.dropReorder()
            }

            onCentroidChanged: if (active && root.pointerDragging)
                                   root.dragToScenePosition(centroid.scenePosition.y)
        }

        delegate: PlayerQueueRow {
            required property int index
            required property var model

            readonly property int sourceRow: root.outline.sourceRowAt(index)

            width: ListView.view.width
            overlay: root.overlay
            entry: model.item
            entryType: model.itemType
            title: model.displayTitle
            subtitle: model.displaySubtitle
            episodeCode: model.episodeCode
            genericEpisodeTitle: model.genericEpisodeTitle
            playable: model.playable
            progress: model.progress
            position: sourceRow + 1
            outlineKind: model.outlineKind
            groupLabel: model.groupLabel
            groupDetail: model.groupDetail
            groupCount: model.groupCount
            inGroup: model.inGroup
            userQueuedRunStart: model.userQueuedRunStart
            current: sourceRow === root.queue.currentIndex
            highlighted: index === list.currentIndex && list.activeFocus
            grabbed: root.grabbedRow >= 0 && sourceRow >= root.grabbedRow && sourceRow < root.grabbedRow
                     + root.grabbedCount
            reordering: root.reordering
            pointerEnabled: !Platform.isTV && root.editable

            onActivated: {
                list.currentIndex = index
                App.playQueueItem(sourceRow)
            }
            onRemoveRequested: App.removeQueueItem(sourceRow)
            onToggleRequested: {
                list.currentIndex = index
                root.toggleCurrentGroup()
            }
        }
    }

    // Always in reach, whatever the queue is doing above it: the one way in
    // to putting something new in the queue without leaving playback.
    ActionButton {
        id: addButton

        anchors.left: list.left
        anchors.right: list.right
        anchors.bottom: parent.bottom
        anchors.bottomMargin: list.inset
        kind: "primary"
        iconName: "add"
        text: "Add to queue"
        onClicked: root.addRequested()

        Keys.onUpPressed: event => {
            InputKeys.focus(list)
            event.accepted = true
        }
    }
}
