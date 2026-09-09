pragma Singleton
import QtQuick

QtObject {
    // The viewport the shell is drawn into, pushed here by AppShell. Every
    // size in the app is a multiple of the yardstick these two numbers make,
    // so there is one place to reason about how big anything should be.
    //
    // Only ever read before the shell exists, which is why the literals do no
    // harm: AppShell binds both, and a binding is evaluated in time for the
    // first frame where the change handlers it replaced were not.
    //
    // Not seeded from Screen. That is an attached property and there is no
    // Item here for it to attach to, so it reads back as nothing and the
    // fallback is what would be used anyway -- an expression that looks like
    // it consults the display while never once doing so.
    property real viewportWidth: 1920
    property real viewportHeight: 1080
    // Set by the shell when the last thing to touch the app was a finger.
    // Taps need a floor that a remote and a mouse do not.
    property bool coarsePointer: false
    // A finger-driven app should not paint or retain a keyboard selection.
    // The first directional/key input turns this back on.
    property bool keyboardFocusActive: true
    // Whether a pointer is the thing currently driving the interface. A
    // television's pointer hides itself without sending a leave event, so a
    // hover can otherwise stay painted on a control nobody is pointing at.
    property bool pointerActive: false
    // Pixels per millimetre, pushed by the shell. Every other size in this file
    // is a pure function of the viewport, which is the right yardstick for
    // reading: a phone is held about half as far away as a monitor, so type
    // half the size subtends the same angle. A fingertip is not held closer,
    // so the things you touch are the one thing that has to be measured in
    // millimetres or a dense panel shrinks them out of reach.
    property real pixelsPerMm: 3.8

    // The user's zoom, pushed here by the shell alongside the viewport. This
    // file is a pure function of what it is handed, so it can be reasoned
    // about — and tested — without the rest of the app standing around it.
    property int zoomPercent: 100
    readonly property int uiScalePercent: Math.max(50, Math.min(180, zoomPercent))
    readonly property real uiScale: uiScalePercent / 100

    // The viewport that scores 1.0, as a geometric mean. A viewport's
    // geometric mean is its shape in one number, so a square panel and a
    // widescreen one of the same area land on the same sizes and neither
    // needs an aspect ratio named anywhere.
    //
    // Its unit is whatever Qt calls a logical pixel, and that is not the same
    // thing everywhere: on a desktop it is roughly a 96dpi pixel, so the
    // default is sqrt(1920 * 1080), the viewport this file was drawn against.
    // On Android it is exactly an Android dp, which is a different and better
    // yardstick -- the platform has already accounted for how far away the
    // panel is held -- so the shell substitutes Android's own. Set by
    // AppShell; see the binding there for why.
    property real baselinePx: 1440
    readonly property real viewportRatio: Math.sqrt(Math.max(1, viewportWidth) * Math.max(1, viewportHeight))
                                          / baselinePx

    // Two ramps off the one yardstick, because type and artwork want
    // different things from a bigger screen.
    //
    // Type, chrome and spacing climb slowly — a viewport twice as wide reads
    // about a quarter larger — because legibility has a floor and a ceiling
    // that resolution does not move. The floor keeps a phone readable; the
    // ceiling stops a 4K panel turning the interface into furniture.
    readonly property real viewportScale: Math.max(0.82, Math.min(1.5, Math.cbrt(viewportRatio)))
    // Artwork climbs faster, because a larger panel is watched from the same
    // distance and posters should gain size rather than only multiply. This
    // is the ramp the old per-resolution card table encoded by hand.
    readonly property real artworkScale: Math.max(0.7, Math.min(1.9, Math.pow(viewportRatio, 0.6)))

    readonly property real scale: viewportScale * uiScale
    readonly property real cardScale: artworkScale * uiScale

    // Chrome that floats over content sits a little under the page behind it,
    // and is capped so a very large window does not push it out of
    // proportion to the picture it is drawn on.
    readonly property real chromeBaselineScale: 0.78
    readonly property real chromeScale: Math.max(0.65, Math.min(1, chromeBaselineScale * Math.cbrt(viewportRatio)))

    readonly property int topBarHeightPx: scaled(62)
    readonly property int pageMarginPx: pageMargin(viewportWidth)
    readonly property int gapPx: scaled(22)
    // A finger needs about nine millimetres whatever the yardstick says.
    // The 44 keeps the floor the app has always had on a ~96 dpi touchscreen,
    // where nine millimetres works out smaller; a dense panel takes the
    // millimetres instead.
    readonly property int touchTargetPx: coarsePointer ? Math.max(44, Math.round(9 * pixelsPerMm)) : 0
    readonly property int controlHeightPx: Math.max(touchTargetPx, scaled(48))
    // The selection ring is read from touching distance, so it gets a physical
    // floor too — two pixels is a hairline on a phone panel.
    readonly property int focusRingPx: Math.max(scaled(2), coarsePointer ? Math.round(0.5 * pixelsPerMm) : 0)
    // The panel density Qt's flick defaults were drawn against, near enough.
    readonly property real referencePixelsPerMm: 3.8
    // Touch flings need to coast between swipes rather than brake like remote
    // navigation. Keep the physical distance consistent across panel densities.
    readonly property real densityRatio: Math.max(1, pixelsPerMm / referencePixelsPerMm)
    readonly property int flickDecelerationPx: Math.round((coarsePointer ? 750 : 1500) * densityRatio)
    readonly property int maximumFlickVelocityPx: Math.round((coarsePointer ? 6000 : 2500) * densityRatio)
    readonly property int sectionGapPx: scaled(28)
    readonly property int iconSizePx: scaled(22)
    readonly property int titleSizePx: scaled(34)
    readonly property int bodySizePx: scaled(16)
    readonly property int metaSizePx: scaled(14)
    readonly property real landscapeCardRatio: 1.6

    function scaled(value) {
        return Math.max(1, Math.round(value * scale))
    }
    function cardScaled(value) {
        return Math.max(1, Math.round(value * cardScale))
    }
    function chromeScaled(value) {
        return Math.max(1, Math.round(value * chromeScale * uiScale))
    }
    // Pixels back into yardstick units. This is the only honest way to ask
    // how much room a container has, because it is free of both the panel's
    // density and the user's zoom.
    function units(px) {
        return px / Math.max(0.01, scale)
    }

    // The one sanctioned threshold in the app. It is measured in units of the
    // container being laid out, never of the device, so the same view lands
    // in "compact" inside a player-side panel and in "wide" on a television
    // without either of them naming a resolution.
    function lane(width) {
        const u = units(width)
        if (u < 700)
            return "compact"
        return u < 1360 ? "regular" : "wide"
    }
    function laneAtLeast(width, minimum) {
        const order = {
            "compact": 0,
            "regular": 1,
            "wide": 2
        }
        return order[lane(width)] >= order[minimum]
    }

    // Margins give room back to content as a container narrows, rather than
    // holding a page-sized gutter either side of a phone-sized column.
    function pageMargin(width) {
        return Math.max(1, Math.min(scaled(30), Math.round(width * 0.06)))
    }
    function detailRowPosterWidth() {
        return cardScaled(176)
    }
    function detailHeroHeight(height) {
        return Math.max(scaled(420), Math.min(scaled(660), Math.round(height * 0.64)))
    }
    // One card width serves the library grid and every row that quotes it, so a
    // home row and the grid behind it land on the same size at any window size
    // rather than each rounding a table of their own.
    function cardWidth(width) {
        const count = columns(width)
        const available = Math.max(1, width - pageMargin(width) * 2)
        return Math.max(1, Math.floor((available - gapPx * (count - 1)) / count))
    }
    // A 16:9 card beside a 2:3 one at landscapeCardRatio stands exactly three
    // fifths as tall, which is the proportion the rows have always had.
    function landscapeCardWidth(width) {
        return Math.round(cardWidth(width) * landscapeCardRatio)
    }
    function columns(width) {
        const targetWidth = cardScaled(190)
        const available = Math.max(targetWidth, width - pageMargin(width) * 2)
        return Math.max(1, Math.min(12, Math.floor((available + gapPx) / (targetWidth + gapPx))))
    }
    // Dropdowns and pickers are read at arm's length whatever they hang off,
    // so they take a share of the container and stop where a line of menu
    // text stops being comfortable to scan.
    function menuPanelWidth(width, preferred) {
        return Math.max(scaled(240), Math.min(scaled(preferred), Math.round(width * 0.42)))
    }
}
