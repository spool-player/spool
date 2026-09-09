pragma ComponentBehavior: Bound

import QtQuick
import "../theme"
import "../primitives"
import "../primitives/ModelAccess.js" as ModelAccess

// A vertical stack of horizontal media rows, walked top to bottom by a
// remote and scrolled by a wheel or a finger.
//
// The home page and the search results page each grew their own copy of
// this: the same ListView of MediaRows, the same skip-the-empty-rows
// movement, the same handing focus back up at the top edge, and the same
// dance to recover a sensible focus after a pointer scrolled the view out
// from under it. They now share one, which is also what the sheet over the
// player shows for Home.
//
// Sections are plain descriptors, so a caller can build them from anything:
//   { key, title, model, kind, useSeriesPoster, preferEpisodeTitle,
//     contextSource }
FocusScope {
    id: root

    property var shell
    property var sections: []
    property string contextReturnRoute: ""
    property bool navigationFocusVisible: true
    // Set on the first row so the shell can time the first frame it draws.
    property bool measureFirstRow: false
    property bool firstRowReady: false
    property real rowSpacing: Metrics.scaled(14)
    // Anything the host wants above the rows (a person's portrait) or below
    // them (an empty state that fills the remaining height).
    property Component header: null
    property Component footer: null

    // Which section holds focus. Counted over every section, not only the
    // visible ones, so it survives a row emptying and refilling.
    property int currentSection: -1

    property bool pointerNavigationPending: false
    property var pendingScrollController: null

    signal activated(var section, int index, var item)
    signal contextRequested(var section, int index, var item, var anchor)
    // Nothing above the first row: the host decides where focus goes, which
    // is the navigation bar on a page and the search field on a sheet.
    signal edgeUp

    readonly property alias count: sectionList.count
    readonly property alias contentY: sectionList.contentY

    readonly property int sectionCount: sections ? sections.length : 0

    function sectionAt(index) {
        return index >= 0 && index < sectionCount ? sections[index] : null
    }

    function rowAt(index) {
        return index >= 0 && index < sectionList.count ? sectionList.itemAtIndex(index) : null
    }

    function currentRow() {
        return rowAt(currentSection)
    }

    // A row is worth stopping on when it has something to show. MediaRow
    // already answers that, including the case where it is holding space for
    // a load in flight.
    function isPopulated(index) {
        const row = rowAt(index)
        if (row)
            return row.count > 0
        const section = sectionAt(index)
        return Boolean(section) && ModelAccess.count(section.model) > 0
    }

    function nextPopulated(start, direction) {
        for (let index = start; index >= 0 && index < sectionCount; index += direction) {
            if (isPopulated(index))
                return index
        }
        return -1
    }

    function focusCurrentSection() {
        const row = currentRow()
        if (!row || !row.focusList())
            return false
        sectionList.positionViewAtIndex(currentSection, ListView.Contain)
        return true
    }

    function focusSection(index) {
        if (!isPopulated(index))
            return false
        currentSection = index
        return focusCurrentSection()
    }

    // Focus the first row of a given kind, falling back to the first row
    // there is. Search uses this to return you to the results you were last
    // looking at rather than always to Movies.
    function focusPreferred(key) {
        for (let index = 0; index < sectionCount; ++index) {
            const section = sectionAt(index)
            if (section && String(section.key || "") === String(key) && isPopulated(index))
                return focusSection(index)
        }
        const first = nextPopulated(0, 1)
        return first >= 0 && focusSection(first)
    }

    function moveSection(direction) {
        const next = nextPopulated(currentSection + direction, direction)
        if (next < 0) {
            if (direction < 0)
                root.edgeUp()
            return true
        }
        currentSection = next
        sectionList.positionViewAtIndex(next, ListView.Contain)
        Qt.callLater(focusCurrentSection)
        return true
    }

    function positionAtBeginning() {
        sectionList.positionViewAtBeginning()
    }

    function reset() {
        firstRowReady = false
        currentSection = nextPopulated(0, 1)
        Qt.callLater(focusCurrentSection)
    }

    // Keep focus somewhere sensible when rows repopulate underneath it.
    function repair() {
        if (!isPopulated(currentSection)) {
            const next = nextPopulated(0, 1)
            if (next < 0)
                return false
            currentSection = next
        }
        return activeFocus && Metrics.keyboardFocusActive && !pointerNavigationPending ? focusCurrentSection() : true
    }

    function currentItem() {
        const row = currentRow()
        const section = sectionAt(currentSection)
        return row && section ? ModelAccess.at(section.model, row.currentIndex) : ({})
    }

    function clearPendingPointerNavigation() {
        pointerNavigationPending = false
        pendingScrollController = null
    }

    function beginPointerNavigation(controller) {
        // A wheel can cross several rows in one gesture. Each row has a
        // handler, but only one animation may write this page's contentY.
        if (pendingScrollController && pendingScrollController !== controller)
            pendingScrollController.stopScrolling()
        if (controller)
            pendingScrollController = controller
        pointerNavigationPending = true
        navigationFocusVisible = false
    }

    // After a scroll, the focused card is usually off screen. Rather than
    // yanking the view back to it, adopt whatever card the scroll left in the
    // top-left corner and carry on from there.
    function topLeftVisibleCard() {
        sectionList.forceLayout()
        let best = null
        for (let index = 0; index < sectionList.count; ++index) {
            const row = sectionList.itemAtIndex(index)
            if (!row || !row.topLeftVisibleCandidate)
                continue
            const card = row.topLeftVisibleCandidate(sectionList)
            if (!card)
                continue
            const candidate = {
                "index": card.index,
                "top": card.top,
                "left": card.left,
                "rowIndex": index,
                "row": row
            }
            if (!best || InputKeys.earlierVisibleCandidate(candidate, best))
                best = candidate
        }
        return best
    }

    function recoverPointerNavigation() {
        if (pendingScrollController && pendingScrollController.stopScrolling)
            pendingScrollController.stopScrolling()
        sectionList.cancelFlick()
        const candidate = topLeftVisibleCard()
        if (!candidate)
            return true
        const contentY = sectionList.contentY
        if (!InputKeys.focusIndexWithoutScrolling(sectionList, candidate.rowIndex))
            return true
        if (!candidate.row.focusIndexWithoutScrolling(candidate.index))
            return true
        sectionList.contentY = contentY
        currentSection = candidate.rowIndex
        navigationFocusVisible = true
        clearPendingPointerNavigation()
        return true
    }

    function routeKey(key, phase, repeat) {
        if (phase !== "release" && InputKeys.isDirection(key) && pointerNavigationPending)
            return recoverPointerNavigation()
        const row = currentRow()
        if (!row)
            return false
        if (key === Qt.Key_Up || key === Qt.Key_Down)
            return moveSection(key === Qt.Key_Down ? 1 : -1)
        return row.routeKey(key, phase, repeat)
    }

    function activate() {
        const row = currentRow()
        if (row && row.currentIndex >= 0)
            root.activated(sectionAt(currentSection), row.currentIndex, currentItem())
    }

    function longPress() {
        const row = currentRow()
        return Boolean(row && row.longPress && row.longPress())
    }

    onActiveFocusChanged: {
        clearPendingPointerNavigation()
        if (activeFocus && Metrics.keyboardFocusActive) {
            navigationFocusVisible = true
            Qt.callLater(focusCurrentSection)
        }
    }

    ListView {
        id: sectionList

        anchors.fill: parent
        model: root.sections
        header: root.header
        footer: root.footer
        spacing: root.rowSpacing
        clip: true
        reuseItems: true
        cacheBuffer: 0
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick
        flickDeceleration: Metrics.flickDecelerationPx
        maximumFlickVelocity: Metrics.maximumFlickVelocityPx
        keyNavigationEnabled: false
        focus: true

        FastWheelHandler {
            id: wheel
            flickable: sectionList
            onScrolled: root.beginPointerNavigation(wheel)
        }

        onDraggingChanged: if (dragging)
        root.beginPointerNavigation(null)

        delegate: MediaRow {
            id: mediaRow

            required property int index
            required property var modelData

            width: sectionList.width
            title: String(modelData.title || "")
            model: modelData.model
            shell: root.shell
            cardKind: String(modelData.kind || "poster")
            useSeriesPoster: Boolean(modelData.useSeriesPoster)
            preferEpisodeTitle: Boolean(modelData.preferEpisodeTitle)
            enabledRow: modelData.enabled === undefined ? true : Boolean(modelData.enabled)
            reserveWhenEmpty: Boolean(modelData.reserveWhenEmpty)
            loading: Boolean(modelData.loading)
            emptyText: String(modelData.emptyText || "")
            focusVisible: root.navigationFocusVisible
            // This view is already inset by its host; use its usable width.
            cardWidth: Math.round(Metrics.rowCardWidth(root.width) * (cardKind === "poster" || cardKind === "square"
                                                                      ? 1 : Metrics.landscapeCardRatio))

            cardGap: Metrics.gapPx
            wheelFlickable: sectionList
            atomicPopulate: root.measureFirstRow && index === 0
            itemContextSource: String(modelData.contextSource || modelData.key || "")
            itemContextReturnRoute: root.contextReturnRoute

            onVerticalWheelScrolled: controller => root.beginPointerNavigation(controller)
            onPointerSelected: {
                root.clearPendingPointerNavigation()
                root.currentSection = index
            }
            onDelegatesPresentedChanged: if (root.measureFirstRow && index === 0 && delegatesPresented)
            root.firstRowReady = true
            onActivated: (itemIndex, item) => {
                root.currentSection = index
                root.activated(modelData, itemIndex, item)
            }
        }
    }
}
