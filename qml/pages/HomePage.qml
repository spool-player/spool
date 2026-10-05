pragma ComponentBehavior: Bound

import QtQuick
import "../theme"
import "../browse"
import "../shell/ItemActivation.js" as ItemActivation

FocusScope {
    id: root

    property var shell
    property var uiTransitionToken: 0
    // An all-hidden library row has management controls instead of cards.
    readonly property bool contentReady: rows.firstRowReady || !Providers.hasAccounts || (Libraries.count === 0 &&
                                                                                          !Home.loading)

    focus: true

    // With one server every library is obviously its; with several, each
    // says whose it is: the provider's mark, the server, and where it is.
    function sourceBadge(scopedId, withDetail) {
        const origin = Sources.originOf(String(scopedId || ""))
        if (!origin || !origin.serverName)
            return null
        return {
            "iconUrl": origin.iconUrl,
            "text": origin.serverName,
            "detail": withDetail ? (origin.address || origin.providerName) : ""
        }
    }

    function buildSections() {
        const multiple = Sources.multipleSources
        const hidden = Libraries.hiddenLibraries
        const sections = [
                  {
                      "key": "libraries",
                      "title": "Libraries",
                      "model": Libraries,
                      "kind": "library",
                      "moveItem": (from, to) => Libraries.moveLibrary(from, to),
                      "contextMenu": (library, anchor, context) => root.shell ? root.shell.openLibraryMenu(library,
                                                                                                           anchor, context) :
                                                                                false,
                      "reserveWhenEmpty": hidden.length > 0,
                      "headerActionText": "Show hidden libraries",
                      "headerAction": hidden.length > 0 ? (() => root.shell ? root.shell.openLibraryMenu(null, null, {
                                                                                                             "showHidden":
                                                                                                             true
                                                                                                         }) : false) :
                                                          null,
                      "cardBadge": multiple ? (library => root.sourceBadge(library.libraryId, true)) : null
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
                              "enabled": !Libraries.isHidden(String(row && row.libraryId || "")),
                              "reserveWhenEmpty": Home.loading,
                              "headerBadge": multiple && row ? root.sourceBadge(row.libraryId, false) : null,
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
