pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"
import "../browse"
import "../shell/ItemActivation.js" as ItemActivation
import "../shell/RoutePolicy.js" as RoutePolicy

FocusScope {
    id: root

    property var shell
    readonly property bool directionRelease: rows.directionRelease
    readonly property var person: RoutePolicy.personContext(shell ? shell.routeArgs : ({}))
    readonly property bool routeActive: Boolean(shell && shell.route === "personDetails")
    property bool routeRefreshScheduled: false
    // The controller supplies title/model/kind per credit row; the page adds
    // the presentation the row kind implies. A person's episode credits name
    // the episode, where Continue Watching names the series.
    readonly property var sections: {
        const source = Content.personItemRows || []
        const result = []
        for (let index = 0; index < source.length; ++index) {
            const row = source[index]
            const episodeRow = String((row && row.kind) || "") === "landscape"
            result.push({
                            "key": "credit" + index,
                            "title": String((row && row.title) || ""),
                            "model": row ? row.model : null,
                            "kind": episodeRow ? "landscape" : "poster",
                            "preferEpisodeTitle": episodeRow,
                            "contextSource": "person"
                        })
        }
        return result
    }
    readonly property int contentMargin: Metrics.pageMarginPx
    readonly property int portraitWidth: Math.min(Metrics.scaled(176), Math.max(Metrics.scaled(128), width * 0.1))
    readonly property bool contentReady: !Content.personItemsBusy
    focus: true

    Component.onCompleted: {
        rows.reset()
        schedulePersonRefresh()
    }
    onSectionsChanged: rows.reset()
    onPersonChanged: schedulePersonRefresh()
    onRouteActiveChanged: schedulePersonRefresh()

    function schedulePersonRefresh() {
        if (!routeActive || routeRefreshScheduled)
            return
        routeRefreshScheduled = true
        Qt.callLater(function () {
            root.routeRefreshScheduled = false
            if (root.routeActive)
                Content.loadPersonItems(String(root.person.id || ""))
        })
    }

    function routeKey(key, phase, repeat) {
        return rows.routeKey(key, phase, repeat)
    }

    function openAt(section, index, item) {
        ItemActivation.open(item, {
                                "source": "person",
                                "returnRoute": "personDetails",
                                "browseRoute": "libraryGrid"
                            }, App, shell, section ? section.model : null, index)
    }

    function activate() {
        rows.activate()
    }

    function longPress() {
        return rows.longPress()
    }

    function currentMediaItem() {
        return rows.currentItem()
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    RowStackView {
        id: rows

        anchors.fill: parent
        anchors.margins: root.contentMargin
        sections: root.sections
        shell: root.shell
        contextReturnRoute: "personDetails"
        rowSpacing: Metrics.sectionGapPx
        focus: true

        // Leaving the top rows brings the portrait back into view before
        // handing focus up, so you can see whose credits you were reading.
        onEdgeUp: {
            positionAtBeginning()
            if (root.shell)
                root.shell.focusNavBar()
        }
        onActivated: (section, index, item) => root.openAt(section, index, item)

        header: RowLayout {
            width: rows.width
            height: Math.max(Metrics.scaled(172), root.portraitWidth * 1.22) + Metrics.scaled(22)
            spacing: Metrics.scaled(22)

            ImageCard {
                Layout.preferredWidth: root.portraitWidth
                Layout.preferredHeight: Math.round(root.portraitWidth * 1.22)
                Layout.alignment: Qt.AlignTop
                imageUrl: Art.url(root.person, "poster")
                fallbackIcon: "person"
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                spacing: Metrics.scaled(9)

                AppText {
                    Layout.fillWidth: true
                    text: root.person.name || "Person"
                    font.pixelSize: Math.min(Metrics.scaled(46), Metrics.titleSizePx + Metrics.scaled(8))
                    font.weight: Font.DemiBold
                    maximumLineCount: 2
                    wrapMode: Text.Wrap
                }

                TechMetadataLine {
                    Layout.fillWidth: true
                    metadata: [root.person.type || "", root.person.role || ""].filter(function (value) {
                        return value.length > 0
                    }).join(" / ")
                }
            }
        }

        footer: EmptyPlaceholder {
            width: rows.width
            height: rows.height
            visible: root.sections.length === 0 && !Content.personItemsBusy
            title: "No items"
            detail: "No matching credits in your libraries."
        }
    }
}
