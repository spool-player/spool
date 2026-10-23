pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

OverlayDialog {
    id: root

    preferredWidth: 1120
    preferredHeight: 800
    padding: 28
    panelColor: Theme.bgRaised
    property var item: ({})
    property var shell
    property int currentActionIndex: 0
    readonly property string itemId: item && item.movieId ? String(item.movieId) : ""
    readonly property var detail: Content.detailItem && String(Content.detailItem.movieId || "") === itemId
                                  ? Content.detailItem : ({})
    readonly property var sources: detail.mediaSources || []
    readonly property int closeActionIndex: 0
    readonly property int actionCount: 1

    signal closed
    onDismissed: closeOverlay()

    onVisibleChanged: if (visible) {
                          currentActionIndex = closeActionIndex
                          Qt.callLater(ensureFocus)
                          Qt.callLater(refreshItemDetail)
                      }
    onItemChanged: if (visible)
                       Qt.callLater(refreshItemDetail)
    onActiveFocusChanged: if (visible && !activeFocus)
                              InputKeys.focus(root)

    component Pair: ColumnLayout {
        property string label: ""
        property string value: ""
        visible: value.length > 0
        spacing: 3
        SecondaryText {
            Layout.fillWidth: true
            text: label
            color: Theme.textMuted
            font.pixelSize: Metrics.metaSizePx - 1
            elide: Text.ElideRight
        }
        AppText {
            Layout.fillWidth: true
            text: value
            color: Theme.textPrimary
            font.pixelSize: Metrics.bodySizePx
            font.weight: Font.Medium
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
        }
    }

    function text(value) {
        return value === undefined || value === null ? "" : String(value)
    }
    function upper(value) {
        const v = text(value)
        return v.length > 0 ? v.toUpperCase() : ""
    }
    function titleText() {
        return item && (item.displayTitle || item.title) ? (item.displayTitle || item.title) : "Media info"
    }
    function episodeCode() {
        if (!item || item.itemType !== "Episode")
            return ""
        const season = Number(item.seasonNumber || 0)
        const episode = Number(item.episodeNumber || 0)
        if (season > 0 && episode > 0)
            return "S" + String(season).padStart(2, "0") + "E" + String(episode).padStart(2, "0")
        const match = String(item.displaySubtitle || item.subtitle || "").match(/S(\d+)\s*:?\s*E(\d+)/i)
        return match ? "S" + match[1].padStart(2, "0") + "E" + match[2].padStart(2, "0") : ""
    }
    function titleMetadata() {
        if (item && item.itemType === "Episode")
            return episodeCode()
        return [item && item.itemType ? item.itemType : "", item && item.year > 0 ? String(item.year) : ""].filter(
                    function (value) {
                        return value.length > 0
                    }).join(" · ")
    }
    function bitrate(bits) {
        const v = Number(bits || 0)
        if (v <= 0)
            return ""
        if (v >= 1000000)
            return (v / 1000000).toFixed(v >= 10000000 ? 0 : 1) + " Mbps"
        if (v >= 1000)
            return Math.round(v / 1000) + " kbps"
        return v + " bps"
    }
    function fileSize(bytes) {
        let value = Number(bytes || 0)
        if (value <= 0)
            return ""
        const units = ["B", "KB", "MB", "GB", "TB"]
        let unit = 0
        while (value >= 1024 && unit < units.length - 1) {
            value /= 1024;
            ++unit
        }
        return value.toFixed(unit >= 3 ? 2 : unit === 0 ? 0 : 1) + " " + units[unit]
    }
    function runtime(ticks) {
        const minutes = Math.round(Number(ticks || 0) / 600000000)
        if (minutes <= 0)
            return ""
        const hours = Math.floor(minutes / 60)
        const mins = minutes % 60
        return hours > 0 ? hours + "h " + mins + "m" : mins + "m"
    }
    function codec(stream) {
        const parts = []
        if (stream.codec)
            parts.push(upper(stream.codec))
        if (stream.profile)
            parts.push(stream.profile)
        return parts.join(" / ")
    }
    function streamSummary(stream) {
        const parts = []
        const c = codec(stream)
        if (c.length > 0)
            parts.push(c)
        if (stream.width > 0 && stream.height > 0)
            parts.push(stream.width + "x" + stream.height)
        if (stream.frameRate > 0)
            parts.push(Number(stream.frameRate).toFixed(3).replace(/0+$/, "").replace(/\.$/, "") + " fps")
        if (stream.channels > 0)
            parts.push(stream.channels === 1 ? "Mono" : stream.channels === 2 ? "Stereo" : stream.channels
                                                                                + " channels")
        if (stream.sampleRate > 0)
            parts.push((Number(stream.sampleRate) / 1000).toFixed(stream.sampleRate % 1000 === 0 ? 0 : 1) + " kHz")
        if (stream.bitDepth > 0)
            parts.push(stream.bitDepth + "-bit")
        if (stream.videoRange)
            parts.push(stream.videoRange)
        if (stream.pixelFormat)
            parts.push(stream.pixelFormat)
        if (stream.language)
            parts.push(stream.language)
        if (stream.title)
            parts.push(stream.title)
        const br = bitrate(stream.bitRate)
        if (br.length > 0)
            parts.push(br)
        return parts.join(" · ")
    }
    function streamFlags(stream) {
        const flags = []
        if (stream.index >= 0)
            flags.push("Stream " + stream.index)
        if (stream.isDefault)
            flags.push("Default")
        if (stream.isForced)
            flags.push("Forced")
        if (stream.isExternal)
            flags.push("External")
        if (stream.isInterlaced)
            flags.push("Interlaced")
        return flags.join(" · ")
    }
    function sourcePairs(source) {
        return [
                    {
                        label: "Container",
                        value: upper(source.container)
                    },
                    {
                        label: "Protocol",
                        value: text(source.protocol)
                    },
                    {
                        label: "Size",
                        value: fileSize(source.size)
                    },
                    {
                        label: "Bitrate",
                        value: bitrate(source.bitRate)
                    },
                    {
                        label: "Runtime",
                        value: runtime(source.runtimeTicks || (item ? item.runtimeTicks : 0))
                    },
                    {
                        label: "Video type",
                        value: text(source.videoType)
                    },
                    {
                        label: "Path",
                        value: source.path || (item && item.path ? item.path : "")
                    }
                ].filter(function (pair) {
                    return pair.value.length > 0
                })
    }
    function actionAt(index) {
        return closeBtn
    }
    function focusAction(index) {
        currentActionIndex = Math.max(0, Math.min(actionCount - 1, index))
        const target = actionAt(currentActionIndex)
        if (target)
            InputKeys.focus(target)
    }
    function ensureFocus() {
        InputKeys.focus(root)
        focusAction(currentActionIndex)
    }
    function refreshItemDetail() {
        if (itemId.length > 0)
            Content.loadItemDetail(itemId)
    }
    function closeOverlay() {
        root.closed()
    }
    function activateCurrent() {
        closeOverlay()
    }
    function scrollInfo(delta) {
        infoWheel.stopScrolling()
        infoFlick.cancelFlick()
        const first = infoFlick.originY
        const last = first + Math.max(0, infoFlick.contentHeight - infoFlick.height)
        infoFlick.contentY = Math.max(first, Math.min(last, infoFlick.contentY + delta))
    }
    function routeKey(key, phase, repeat) {
        if (phase === "release" && key === Qt.Key_I) {
            closeOverlay()
            return true
        }
        const scrollKey = InputKeys.isVertical(key) || key === Qt.Key_PageUp || key === Qt.Key_PageDown || key
              === Qt.Key_Home || key === Qt.Key_End
        if (phase === "release")
            return scrollKey || InputKeys.isHorizontal(key)
        if (scrollKey) {
            const step = Math.max(Metrics.controlHeightPx, Math.round(infoFlick.height * 0.15))
            const page = Math.round(infoFlick.height * 0.85)
            const delta = key === Qt.Key_Up ? -step : key === Qt.Key_Down ? step : key === Qt.Key_PageUp ? -page : key
                                                                                                           === Qt.Key_PageDown
                                                                                                           ? page : key
                                                                                                             === Qt.Key_Home
                                                                                                             ? -infoFlick.contentHeight :
                                                                                                               infoFlick.contentHeight
            scrollInfo(delta)
            return true
        }
        if (InputKeys.isHorizontal(key)) {
            focusAction(closeActionIndex)
            return true
        }
        return false
    }

    function activate() {
        activateCurrent()
    }

    function back() {
        closeOverlay()
        return true
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 12
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 5
            AppText {
                Layout.fillWidth: true
                text: root.titleText()
                font.pixelSize: Metrics.titleSizePx
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }
            TechMetadataLine {
                Layout.fillWidth: true
                metadata: root.titleMetadata()
            }
        }
        ActionButton {
            id: closeBtn
            text: "Close"
            iconName: "close"
            focus: root.currentActionIndex === root.closeActionIndex
            onClicked: root.closeOverlay()
            onActiveFocusChanged: if (activeFocus)
                                      root.currentActionIndex = root.closeActionIndex
        }
    }

    Flickable {
        id: infoFlick
        objectName: "mediaInfoScrollView"

        Layout.fillWidth: true
        Layout.fillHeight: true
        contentWidth: width
        contentHeight: infoColumn.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        FastWheelHandler {
            id: infoWheel
            flickable: infoFlick
        }

        ListScrollBar {
            id: infoScrollBar
            parent: infoFlick
            flickable: infoFlick
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            z: 1
        }

        ColumnLayout {
            id: infoColumn
            width: Math.max(0, infoFlick.width - (infoScrollBar.visible ? infoScrollBar.width + Metrics.scaled(8) : 0))
            spacing: 18

            EmptyPlaceholder {
                Layout.fillWidth: true
                visible: root.sources.length === 0
                title: "No media source data"
                detail: "The server sent no codec or stream details for this item."
            }

            Repeater {
                model: root.sources
                Surface {
                    required property var modelData
                    readonly property var source: modelData || ({})
                    Layout.fillWidth: true
                    Layout.preferredHeight: sourceColumn.implicitHeight + 24
                    baseColor: Theme.bgPanel

                    ColumnLayout {
                        id: sourceColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 12
                        spacing: 12
                        SectionHeader {
                            Layout.fillWidth: true
                            title: source.name && source.name.length > 0 ? source.name : "Media source"
                        }
                        GridLayout {
                            Layout.fillWidth: true
                            columns: Metrics.lane(root.width) === "wide" ? 4 : 2
                            columnSpacing: Metrics.scaled(18)
                            rowSpacing: Metrics.scaled(12)
                            Repeater {
                                model: root.sourcePairs(source)
                                Pair {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    label: modelData.label
                                    value: modelData.value
                                }
                            }
                        }
                        Repeater {
                            model: source.streams || []
                            Surface {
                                required property var modelData
                                readonly property var stream: modelData || ({})
                                Layout.fillWidth: true
                                Layout.preferredHeight: streamColumn.implicitHeight + 20
                                baseColor: Theme.bg
                                ColumnLayout {
                                    id: streamColumn
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 6
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 8
                                        MetadataChip {
                                            text: root.text(stream.type || "Stream")
                                            selected: true
                                        }
                                        AppText {
                                            Layout.fillWidth: true
                                            text: stream.displayTitle || root.text(stream.type || "Stream") + " stream"
                                            font.pixelSize: Metrics.bodySizePx
                                            font.weight: Font.DemiBold
                                            elide: Text.ElideRight
                                        }
                                    }
                                    AppText {
                                        Layout.fillWidth: true
                                        text: root.streamSummary(stream)
                                        color: Theme.textSecondary
                                        font.pixelSize: Metrics.bodySizePx - 1
                                        wrapMode: Text.Wrap
                                        maximumLineCount: 3
                                    }
                                    SecondaryText {
                                        Layout.fillWidth: true
                                        visible: text.length > 0
                                        text: root.streamFlags(stream)
                                        color: Theme.textMuted
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
