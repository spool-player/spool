pragma ComponentBehavior: Bound

import QtQuick
import "../theme"
import "../browse"
import "../shell/ItemActivation.js" as ItemActivation

FocusScope {
    id: root

    property var shell
    property var uiTransitionToken: 0
    readonly property bool contentReady: rows.firstRowReady

    focus: true

    function buildSections() {
        const sections = [
                  {
                      "key": "libraries",
                      "title": "Libraries",
                      "model": Libraries,
                      "kind": "library"
                  }
              ]
        sections.push({
                          "key": "resumeItems",
                          "title": "Continue Watching",
                          "model": Home.resumeItems,
                          "kind": "landscape",
                          "reserveWhenEmpty": Home.loading
                      }, {
                          "key": "nextUpItems",
                          "title": "Next Up",
                          "model": Home.nextUpItems,
                          "kind": "landscape",
                          "reserveWhenEmpty": Home.loading && (Home.latestLibraryRows || []).some(row => row.collectionType
                                                                                                         === "tvshows")
                      })
        const latest = Home.latestLibraryRows || []
        for (let index = 0; index < latest.length; ++index) {
            const row = latest[index]
            const rowIndex = Number(row && row.rowIndex !== undefined ? row.rowIndex : index)
            sections.push({
                              "key": "latestLibrary",
                              "title": row && row.title ? row.title : "Recently Added",
                              "model": row && row.model ? row.model : null,
                              "kind": row && row.kind ? row.kind : "poster",
                              "reserveWhenEmpty": Home.loading,
                              "useSeriesPoster": true,
                              "preferEpisodeTitle": true,
                              // The row's position is part of its identity for
                              // route restoration, so it rides in the context.
                              "contextSource": "latestLibrary:" + rowIndex
                          })
        }
        return sections
    }

    // The two rows that are not simply "open this item": resuming plays
    // straight away, and a library is a place rather than a thing.
    function activateAt(section, index, item) {
        const key = String((section && section.key) || "")
        if (key === "resumeItems") {
            App.playFromModel(section.model, index)
            return
        }
        if (key === "libraries") {
            App.openLibrary(index)
            if (shell)
                shell.replaceRoute("libraryGrid", {
                                       "libraryId": String(item.libraryId || ""),
                                       "focusIndex": 0
                                   })
            return
        }
        ItemActivation.open(item, {
                                "source": String((section && section.contextSource) || "nextup"),
                                "returnRoute": "home",
                                "browseRoute": "libraryGrid"
                            }, App, shell, section ? section.model : null, index)
    }

    function routeKey(key, phase, repeat) {
        return rows.routeKey(key, phase, repeat)
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

    Component.onCompleted: rows.reset()

    Connections {
        target: Home
        function onLatestLibraryRowsChanged() {
            Qt.callLater(rows.repair)
        }
    }

    RowStackView {
        id: rows

        anchors.fill: parent
        anchors.leftMargin: Metrics.pageMarginPx
        anchors.rightMargin: Metrics.pageMarginPx
        anchors.topMargin: Metrics.pageMarginPx
        shell: root.shell
        sections: root.buildSections()
        contextReturnRoute: "home"
        measureFirstRow: true
        focus: true

        onEdgeUp: if (root.shell)
        root.shell.focusNavBar()
        onActivated: (section, index, item) => root.activateAt(section, index, item)
        onFirstRowReadyChanged: if (firstRowReady)
        InputLatency.mark(root.uiTransitionToken, "first_delegate")
    }
}
