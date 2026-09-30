import QtQuick
import "../theme"

Item {
    id: root

    property var shell
    required property var item
    property string kind: "poster"
    property string titleOverride: ""
    property string subtitleOverride: ""
    property string imageOverride: ""
    property string fallbackOverride: ""
    property string fallbackIcon: ""
    property color fallbackTint: "transparent"
    property bool focused: false
    property bool moving: false
    property bool showSubtitle: true
    property bool emphasizedTitle: false
    property bool useSeriesPoster: false
    property bool preferEpisodeTitle: false
    property real progress: -1
    property bool artworkVisible: true
    property bool artworkEnabled: true
    // Where the item comes from, shown over its artwork when set.
    property url badgeIcon
    property string badgeText: ""

    readonly property bool posterKind: kind === "poster"
    readonly property bool squareKind: kind === "square"
    readonly property real metadataHeight: showSubtitle && metadataLabel.text.length > 0 ? metadataLabel.implicitHeight :
                                                                                           0
    readonly property real artHeight: width * (squareKind ? 1 : posterKind ? 1.5 : 9 / 16)
    readonly property real titleAvailableHeight: Math.max(0, height - art.height - Metrics.scaled(10) - metadataHeight)
    readonly property real effectiveProgress: playbackProgress()
    readonly property bool artworkReady: art.artworkReady
    readonly property real focusOutlineHeight: Math.min(height - Metrics.scaled(6), (metadataLabel.visible
                                                                                     ? metadataLabel.y
                                                                                       + metadataLabel.implicitHeight :
                                                                                       titleLabel.y
                                                                                       + titleLabel.implicitHeight)
                                                        + Metrics.scaled(7))

    Component.onCompleted: InputLatency.noteDelegate("media_card", 1)
    Component.onDestruction: InputLatency.noteDelegate("media_card", -1)

    function text(field) {
        return item && item[field] !== undefined && item[field] !== null ? String(item[field]) : ""
    }

    function titleText() {
        if (titleOverride.length > 0)
            return titleOverride
        const title = text("title")
        const seriesName = text("seriesName")
        if (preferEpisodeTitle && text("itemType") === "Episode")
            return title
        if (text("itemType") === "Episode" && seriesName.length > 0)
            return seriesName
        return title || seriesName
    }

    function subtitleText() {
        if (subtitleOverride.length > 0)
            return subtitleOverride
        const subtitle = text("subtitle")
        const title = text("title")
        if (text("itemType") === "Episode") {
            if (preferEpisodeTitle)
                return subtitle
            if (subtitle.length > 0 && title.length > 0)
                return subtitle + " · " + title
            if (title.length > 0)
                return title
        }
        const year = Number(item && item.year || 0)
        return subtitle || (year > 0 ? String(year) : "")
    }

    function imageSource() {
        if (imageOverride.length > 0)
            return imageOverride
        const artKind = posterKind && useSeriesPoster && text("itemType") === "Episode" ? "seriesPoster" : kind
        return Art.url(item, artKind)
    }

    function fallbackText() {
        if (fallbackOverride.length > 0)
            return fallbackOverride
        const type = text("itemType")
        if (!posterKind)
            return subtitleText() || type
        const year = Number(item && item.year || 0)
        return year > 0 ? String(year) : (type || "Poster")
    }

    function playbackProgress() {
        if (progress >= 0)
            return Math.max(0, Math.min(1, progress))
        const resumeTicks = Number(item && item.resumeTicks || 0)
        const runtimeTicks = Number(item && item.runtimeTicks || 0)
        return resumeTicks > 0 && runtimeTicks > 0 ? Math.max(0, Math.min(1, resumeTicks / runtimeTicks)) : 0
    }

    ImageCard {
        id: art
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: root.artHeight
        bordered: false
        imageUrl: root.imageSource()
        artworkEnabled: root.artworkEnabled
        fallbackText: root.fallbackText()
        fallbackIcon: root.fallbackIcon.length > 0 ? root.fallbackIcon : root.posterKind ? "movie" : ""
        fallbackTint: root.fallbackTint
        artworkVisible: root.artworkVisible
    }

    Rectangle {
        anchors.left: art.left
        anchors.bottom: art.bottom
        width: art.width * root.effectiveProgress
        height: Metrics.scaled(4)
        visible: !root.posterKind && root.effectiveProgress > 0
        color: Theme.accent
    }

    SourceBadge {
        anchors.left: art.left
        anchors.top: art.top
        anchors.margins: Metrics.scaled(8)
        overlay: true
        iconUrl: root.badgeIcon
        text: root.badgeText
    }

    Rectangle {
        anchors.fill: parent
        anchors.bottomMargin: root.height - root.focusOutlineHeight
        visible: root.moving
        color: "transparent"
        radius: Theme.radiusMedium
        border.width: Theme.focusBorderWidth
        border.color: Theme.accent
    }

    Rectangle {
        anchors.right: art.right
        anchors.bottom: art.bottom
        anchors.margins: Metrics.scaled(8)
        width: moveLabel.implicitWidth + Metrics.scaled(16)
        height: moveLabel.implicitHeight + Metrics.scaled(8)
        radius: Theme.radiusSmall
        color: Theme.bgPanel
        visible: root.moving

        AppText {
            id: moveLabel
            anchors.centerIn: parent
            text: "↔ Move"
            color: Theme.accent
            font.pixelSize: Metrics.metaSizePx
            font.weight: Font.DemiBold
        }
    }

    AppText {
        id: titleLabel
        anchors.top: art.bottom
        anchors.topMargin: Metrics.scaled(8)
        anchors.left: parent.left
        anchors.leftMargin: Metrics.scaled(4)
        anchors.rightMargin: Metrics.scaled(4)
        anchors.right: parent.right
        visible: text.length > 0 && root.titleAvailableHeight > 0
        text: root.titleText()
        font.pixelSize: Metrics.bodySizePx + (root.emphasizedTitle ? Metrics.scaled(2) : 0)
        font.weight: root.emphasizedTitle ? Font.DemiBold : Font.Medium
        color: root.posterKind && !root.focused ? Theme.textSecondary : Theme.textPrimary
        maximumLineCount: root.titleAvailableHeight >= font.pixelSize * 2.25 ? 2 : 1
        elide: Text.ElideRight
    }

    SecondaryText {
        id: metadataLabel
        anchors.top: titleLabel.bottom
        anchors.topMargin: Metrics.scaled(2)
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Metrics.scaled(4)
        anchors.rightMargin: Metrics.scaled(4)
        visible: root.showSubtitle && text.length > 0 && root.height > y
        text: root.subtitleText()
        color: Theme.textMuted
        font.pixelSize: Metrics.metaSizePx
        elide: Text.ElideRight
    }
}
