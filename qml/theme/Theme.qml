pragma Singleton
import QtQuick
import Spool

QtObject {
    readonly property color bg: "#0E0E0E"
    readonly property color bgRaised: "#161616"
    readonly property color bgPanel: "#1C1C1C"
    readonly property color bgHover: "#242424"
    readonly property color border: "#2A2A2A"
    readonly property color borderStrong: "#3A3A3A"

    readonly property color textPrimary: "#EDEDED"
    readonly property color textSecondary: "#C6C6C6"
    readonly property color textMuted: "#9C9C9C"
    readonly property color textDisabled: "#767676"
    readonly property color accentText: "#061017"

    // Jellyfin draws a library with no artwork as its collection glyph on a
    // saturated field. Mirroring that keeps a fresh server recognisable
    // instead of showing an empty card where every other row has a poster.
    readonly property var libraryTints: ["#00A4DC", "#AA5CC3", "#F5C518", "#38B36B", "#E5533D", "#5A6BFF"]

    function libraryIcon(collectionType) {
        switch (String(collectionType || "").toLowerCase()) {
        case "movies":
            return "movie"
        case "tvshows":
            return "tv"
        case "music":
        case "musicvideos":
            return "music_note"
        case "books":
            return "menu_book"
        case "photos":
        case "homevideos":
            return "photo_library"
        case "playlists":
            return "queue_music"
        case "livetv":
            return "live_tv"
        case "boxsets":
            return "collections"
        default:
            return "folder"
        }
    }

    function libraryTint(seed) {
        const text = String(seed || "")
        let hash = 0
        for (let index = 0; index < text.length; ++index)
            hash = (hash * 31 + text.charCodeAt(index)) % 100000
        return libraryTints[hash % libraryTints.length]
    }

    readonly property color paletteBlue: "#00A4DC"
    readonly property color palettePurple: "#AA5CC3"
    property int accentIndex: 0
    readonly property color accent: accentIndex === 1 ? palettePurple : accentIndex === 2 ? "#7E7CFF" : paletteBlue
    readonly property color accentDim: accentIndex === 1 ? "#78408A" : accentIndex === 2 ? "#4F4DA8" : "#0077A0"
    // The palette colour the accent is not currently using.
    readonly property color accentAlternate: accentIndex === 1 ? paletteBlue : palettePurple
    readonly property color accentPanel: accentIndex === 1 ? "#2C1E31" : accentIndex === 2 ? "#20223C" : "#182A32"
    readonly property color success: "#3FB950"
    // A server answering is the ordinary case, not an achievement. Saying so
    // in signal green put the loudest colour on this palette against every
    // healthy row; this one is desaturated to sit among the greys, so the
    // saturated colours are left to the states that actually want attention.
    readonly property color online: "#8FB39A"
    // Work in progress: reaching a server, waiting on an approval elsewhere.
    readonly property color pending: "#E3B341"
    readonly property color errorPanel: "#2A1717"
    readonly property color errorText: "#FFD6D6"

    readonly property color focusedFill: "#30383D"
    readonly property color floatingPanel: "#F0181818"
    readonly property color overlayScrimStrong: "#CC000000"
    readonly property color busyScrim: "#AA0E0E0E"
    readonly property color backdropScrimTop: "#A0000000"
    readonly property color backdropScrimMiddle: "#D20D0D0D"
    readonly property color backdropScrimLeft: "#5A000000"
    readonly property color backdropScrimRight: "#C4000000"

    readonly property int radiusTiny: Metrics.scaled(2)
    readonly property int radiusSmall: Metrics.scaled(4)
    readonly property int radiusMedium: Metrics.scaled(6)
    readonly property int radiusLarge: Metrics.scaled(10)
    readonly property int radiusPanel: Metrics.scaled(12)
    readonly property int focusBorderWidth: Metrics.focusRingPx
    readonly property int hoverBorderWidth: Math.max(1, Metrics.scaled(1))

    // Native text uses the platform rasterizer and honours font hinting.
    // Distance-field rendering ignores hinting, which makes UI-sized text
    // visibly soft on the 1080p webOS scene.
    property int normalTextRenderType: Text.NativeRendering
    // Set by the shell. Without an on-screen keyboard to defer, focusing a
    // text field should always mean you can type into it.
    property bool textEntryFollowsFocus: true
    property bool reducedMotion: false
    property bool antialiasedText: true
    property string technicalMetadataMode: "Always"
    property string sideRailLabels: "On focus"
}
