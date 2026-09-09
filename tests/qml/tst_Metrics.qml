import QtQuick
import QtTest
import "theme-singletons" as Yardstick

// The layout system's contract: one continuous yardstick, no resolution
// gates. These assertions are what stops a per-device lookup table growing
// back — they fail the moment a size steps rather than flows.
TestCase {
    id: testCase
    name: "Metrics"
    visible: true
    when: windowShown
    width: 400
    height: 300

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: 5000
        flickDeceleration: testCase.metrics.flickDecelerationPx
        maximumFlickVelocity: testCase.metrics.maximumFlickVelocityPx
    }

    function test_touchFlingCoastsBetweenSwipes() {
        metrics.coarsePointer = true
        flick.contentY = 0
        flick.flick(0, -900)
        wait(700)
        const betweenSwipes = flick.contentY
        verify(flick.flicking, "a moderate touch fling should coast after lifting the finger")
        wait(100)
        verify(flick.contentY > betweenSwipes, "content keeps moving between swipes")
    }

    function cleanup() {
        flick.cancelFlick()
    }

    readonly property var metrics: Yardstick.Metrics

    function viewport(width, height, zoom) {
        metrics.viewportWidth = width
        metrics.viewportHeight = height
        metrics.zoomPercent = zoom === undefined ? 100 : zoom
    }

    function init() {
        viewport(1920, 1080, 100)
        metrics.coarsePointer = false
        metrics.mobileLayout = false
        metrics.baselinePx = 1440
        metrics.pixelsPerMm = 3.8
    }

    // The reference viewport every size in the app was drawn against. If this
    // drifts, every screenshot in the project is wrong.
    function test_referenceViewportIsUnscaled() {
        compare(metrics.viewportScale, 1)
        compare(metrics.cardScale, 1)
        compare(metrics.scale, 1)
        compare(metrics.topBarHeightPx, 62)
        compare(metrics.pageMarginPx, 30)
        compare(metrics.gapPx, 22)
        compare(metrics.controlHeightPx, 48)
        compare(metrics.sectionGapPx, 28)
        compare(metrics.iconSizePx, 22)
        compare(metrics.titleSizePx, 34)
        compare(metrics.bodySizePx, 16)
        compare(metrics.metaSizePx, 14)
        compare(metrics.columns(1920), 8)
        compare(metrics.detailRowPosterWidth(), 176)
    }

    // The player's chrome sat at 0.78 of the page behind it before the
    // yardstick was unified, and has to keep sitting there.
    function test_chromeKeepsItsBaseline() {
        compare(metrics.chromeScale, 0.78)
    }

    // A square inner screen and a widescreen one of the same area read at the
    // same size. This is what lets the app ship without an aspect-ratio gate.
    function test_shapeDoesNotChangeSize() {
        viewport(1920, 1080)
        const wide = metrics.scale
        viewport(1440, 1440)
        compare(metrics.scale, wide)
        viewport(1080, 1920)
        compare(metrics.scale, wide)
    }

    // Nothing may step. Sweeping a window across the old bucket boundaries
    // (1280, 1920, 3840) used to jump several pixels at once.
    function test_sizesFlowRatherThanStep() {
        const probes = ["topBarHeightPx", "gapPx", "controlHeightPx", "titleSizePx", "bodySizePx", "metaSizePx"]
        for (let width = 320; width <= 4000; width += 1) {
            viewport(width, Math.round(width * 9 / 16))
            for (const name of probes) {
                const value = metrics[name]
                if (width > 320)
                    verify(Math.abs(value - testCase.previous[name]) <= 1, name + " stepped by more than a pixel at "
                           + width)

                testCase.previous[name] = value
            }
        }
    }

    property var previous: ({})

    // Type climbs gently and artwork climbs faster, but both climb: a bigger
    // viewport never yields a smaller size.
    function test_scaleIsMonotonic() {
        let lastScale = 0
        let lastCard = 0
        for (let width = 320; width <= 4000; width += 20) {
            viewport(width, Math.round(width * 9 / 16))
            verify(metrics.scale >= lastScale, "type scale went backwards at " + width)
            verify(metrics.cardScale >= lastCard, "artwork scale went backwards at " + width)
            lastScale = metrics.scale
            lastCard = metrics.cardScale
        }
        verify(metrics.cardScale > metrics.scale, "artwork should outrun type on a large viewport")
    }

    // A phone-width container gets one column; the old floor of two produced
    // a pair of unreadably narrow cards.
    function test_columnsReachOneAndCapAtTwelve() {
        viewport(1920, 1080)
        compare(metrics.columns(430), 1)
        verify(metrics.columns(640) >= 2)
        viewport(3840, 2160)
        verify(metrics.columns(3840) <= 12)
        for (let width = 200; width <= 4000; width += 7)
            verify(metrics.columns(width) >= 1, "columns fell below one at " + width)
    }

    // Cards from one card width: a row and the grid behind it must agree, and
    // a full set of them must fit the container they were measured against.
    function test_cardsFitTheContainerTheyWereMeasuredAgainst() {
        for (let width = 320; width <= 3840; width += 13) {
            const count = metrics.columns(width)
            const used = metrics.cardWidth(width) * count + metrics.gapPx * (count - 1)
            verify(used <= width - metrics.pageMargin(width) * 2, "cards overflowed their container at " + width)
        }
    }

    // Lanes are measured in yardstick units of the container, so the same
    // pixel width lands in a different lane once the user zooms — which is
    // the point: it is about how much room there is, not how many pixels.
    function test_laneFollowsTheContainerNotTheDevice() {
        viewport(1920, 1080, 100)
        compare(metrics.lane(1920), "wide")
        compare(metrics.lane(900), "regular")
        compare(metrics.lane(430), "compact")
        // A panel inside a large window is compact even though the window is not.
        viewport(3840, 2160, 100)
        compare(metrics.lane(3840), "wide")
        compare(metrics.lane(500), "compact")
        // Zooming to television scale takes the same window down a lane.
        viewport(1920, 1080, 150)
        compare(metrics.lane(1920), "regular")
        verify(metrics.laneAtLeast(1920, "regular"))
        verify(!metrics.laneAtLeast(1920, "wide"))
    }

    // A finger needs a floor a remote does not.
    function test_coarsePointerFloorsControlHeight() {
        viewport(480, 1000, 100)
        const remote = metrics.controlHeightPx
        metrics.coarsePointer = true
        verify(metrics.controlHeightPx >= 44)
        verify(metrics.controlHeightPx >= remote)
    }

    // Everything else is a function of the viewport, which is the right
    // yardstick for reading distance. A fingertip is a fixed size, so the
    // things you touch have to grow with the panel's density rather than
    // shrink into it.
    function test_touchTargetsFollowPanelDensity() {
        viewport(1224, 924, 100)
        metrics.coarsePointer = true
        const sparse = metrics.touchTargetPx
        const sparseRing = metrics.focusRingPx
        metrics.pixelsPerMm = 6.8
        verify(metrics.touchTargetPx > sparse)
        verify(metrics.touchTargetPx >= Math.round(9 * 6.8))
        verify(metrics.focusRingPx > sparseRing)
        verify(metrics.controlHeightPx >= metrics.touchTargetPx)
    }

    function test_androidReferenceDensityAt100Percent() {
        viewport(534, 844, 100)
        metrics.baselinePx = 720
        metrics.coarsePointer = true
        metrics.mobileLayout = true
        compare(metrics.columns(534), 3, "the cover-screen reference should fit three posters at 100%")
        const contentWidth = 534 - 2 * metrics.pageMargin(534)
        compare(metrics.rowCardWidth(contentWidth), metrics.cardWidth(534),
                "home rows must not deduct the page margins twice")
        const landscapeWidth = Math.round(metrics.rowCardWidth(contentWidth) * metrics.landscapeCardRatio)
        verify(landscapeWidth < contentWidth * 0.6, "landscape artwork should not fill the whole cover screen")
        viewport(1047, 791, 100)
        verify(metrics.columns(1047) > 3, "wider displays should derive more columns, not use a fixed count")
        viewport(534, 844, 140)
        verify(metrics.columns(534) < 3, "zoom must still control density")
    }

    function test_foldRoundTripUpdatesWithoutRecreatingMetrics() {
        metrics.mobileLayout = true
        metrics.coarsePointer = true
        metrics.baselinePx = 720
        viewport(1047, 791, 100)
        const unfoldedCard = metrics.cardWidth(1047)
        const unfoldedColumns = metrics.columns(1047)
        viewport(534, 844, 100)
        verify(metrics.columns(534) < unfoldedColumns)
        compare(metrics.columns(534), 3)
        metrics.pixelsPerMm = 5
        const coverTarget = metrics.touchTargetPx
        metrics.pixelsPerMm = 7
        verify(metrics.touchTargetPx > coverTarget)
        viewport(1047, 791, 100)
        compare(metrics.cardWidth(1047), unfoldedCard)
        compare(metrics.columns(1047), unfoldedColumns)
    }

    // A remote and a mouse are not fingers, however dense the panel is.
    function test_densityDoesNotEnlargeRemoteTargets() {
        viewport(1920, 1080, 100)
        const ring = metrics.focusRingPx
        metrics.pixelsPerMm = 12
        compare(metrics.touchTargetPx, 0)
        compare(metrics.focusRingPx, ring)
        compare(metrics.controlHeightPx, 48)
    }

    // Zoom and viewport are independent axes and compose.
    function test_zoomComposesWithViewport() {
        viewport(1920, 1080, 100)
        const plain = metrics.bodySizePx
        viewport(1920, 1080, 150)
        compare(metrics.bodySizePx, Math.round(plain * 1.5))
        viewport(1920, 1080, 400)
        compare(metrics.uiScalePercent, 180, "zoom must stay clamped")
    }

    // The floor is a long way below the default because zooming out is how a
    // television that reports a generous density is told to fit more on.
    function test_zoomFloorReachesHalf() {
        viewport(1920, 1080, 50)
        compare(metrics.uiScalePercent, 50)
        viewport(1920, 1080, 10)
        compare(metrics.uiScalePercent, 50, "zoom must stay clamped")
    }
}
