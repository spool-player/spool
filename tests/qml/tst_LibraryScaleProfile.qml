import QtQuick
import QtTest
import Spool

// The library page against a 20,000-title library behind a server with real
// latency: the real page, browse controller and artwork pipeline. Asserts
// only what must always hold -- a held key keeps moving, the view fills when
// it stops, typing settles -- and reports the timings as LIBRARY_PROFILE for
// comparison between builds. Timings are not gated: they measure the machine.
TestCase {
    id: testCase
    name: "LibraryScaleProfile"
    width: 1280
    height: 720
    visible: true
    when: windowShown

    readonly property int rows: 20000
    readonly property int latencyMs: 60
    property var gaps: []
    property double lastTick: 0

    QtObject {
        id: libraries
        function rowCount() {
            return 0
        }
    }
    QtObject {
        id: nativeWindow
        property real systemMemoryBytes: 0
    }
    QtObject {
        id: settings
        property var values: ({})
        property int uiScalePercent: 100
    }

    // How late a 16 ms timer fires is how long the GUI thread was busy.
    Timer {
        id: probe
        interval: 16
        repeat: true
        onTriggered: {
            const now = Date.now()
            if (testCase.lastTick > 0)
                testCase.gaps.push(Math.max(0, now - testCase.lastTick - interval))
            testCase.lastTick = now
        }
    }

    function findGrid(item) {
        if (typeof item.positionViewAtIndex === "function")
            return item
        for (const child of item.children || []) {
            const found = findGrid(child)
            if (found)
                return found
        }
        return null
    }
    function imageCard(item) {
        if (typeof item.artworkSource === "function")
            return item
        for (const child of item.children || []) {
            const found = imageCard(child)
            if (found)
                return found
        }
        return null
    }
    function percentile(values, fraction) {
        if (values.length === 0)
            return 0
        const sorted = values.slice().sort((a, b) => a - b)
        return sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * fraction))]
    }
    function startProbe() {
        gaps = []
        lastTick = 0
        probe.start()
    }
    function stopProbe() {
        probe.stop()
        return {
            "p50": percentile(gaps, 0.5),
            "p95": percentile(gaps, 0.95),
            "max": percentile(gaps, 1)
        }
    }

    function test_holdThroughLargeLibrary() {
        const browse = ArtworkFixture.prepareSparseLibrary(libraries, nativeWindow, settings, rows, latencyMs)
        const component = Qt.createComponent("qrc:/qt/qml/Spool/qml/pages/LibraryGridPage.qml")
        compare(component.status, Component.Ready, component.errorString())
        const page = createTemporaryObject(component, testCase, {
                                               width: testCase.width,
                                               height: testCase.height
                                           })
        verify(page)
        const grid = findGrid(page)
        verify(grid)
        compare(grid.count, rows)
        tryVerify(() => grid.cacheBuffer > 0)
        grid.forceActiveFocus()

        // Hold Down for four seconds at a desktop's thirty repeats a second.
        startProbe()
        verify(page.routeKey(Qt.Key_Down, "press", false))
        const holdStart = Date.now()
        let presses = 1
        let maxRate = 0
        while (Date.now() - holdStart < 4000) {
            wait(33)
            page.routeKey(Qt.Key_Down, "press", true);
            ++presses
            maxRate = Math.max(maxRate, grid.holdRate)
            if (!grid.heldKey)
                break
        }
        const heldThrough = grid.heldKey !== 0
        const travelled = grid.currentIndex
        page.routeKey(Qt.Key_Down, "release", false)
        const hold = stopProbe()
        verify(heldThrough, "holding through the library never stops on its own")
        verify(travelled > 1000, "a four second hold crosses a large part of the library: " + travelled)

        // Letting go: rows on screen load, then their posters paint.
        startProbe()
        const released = Date.now()
        tryVerify(() => ArtworkFixture.sparseRowLoaded(grid.currentIndex), 5000)
        const rowsMs = Date.now() - released
        tryVerify(() => {
            const card = grid.itemAtIndex(grid.currentIndex)
            const picture = card ? imageCard(card) : null
            return picture && picture.artworkReady && !picture.showingFallback
        }, 8000)
        const postersMs = Date.now() - released
        const settle = stopProbe()

        // Typing a prefix a long way from here.
        const seekStart = Date.now()
        verify(browse.seekPrefix("m"))
        tryVerify(() => !browse.seeking, 5000)
        const seekMs = Date.now() - seekStart

        console.info("LIBRARY_PROFILE " + JSON.stringify({
                                                             "rows": rows,
                                                             "serverLatencyMs": latencyMs,
                                                             "travelledRows": travelled,
                                                             "keyRepeats": presses,
                                                             "peakItemsPerSecond": Math.round(maxRate),
                                                             "holdStallP50Ms": hold.p50,
                                                             "holdStallP95Ms": hold.p95,
                                                             "holdStallMaxMs": hold.max,
                                                             "rowsAfterReleaseMs": rowsMs,
                                                             "postersAfterReleaseMs": postersMs,
                                                             "settleStallMaxMs": settle.max,
                                                             "seekMs": seekMs,
                                                             "blockRequests": ArtworkFixture.sparseBlockRequests(),
                                                             "artworkRequests": ArtworkFixture.requestedItems.length
                                                         }))
    }
}
