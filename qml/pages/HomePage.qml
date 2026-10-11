pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../browse"
import "../primitives"
import "../shell/ItemActivation.js" as ItemActivation

FocusScope {
    id: root

    property var shell
    property var uiTransitionToken: 0
    readonly property bool directionRelease: rows.directionRelease
    // An all-hidden library row has management controls instead of cards.
    readonly property bool contentReady: rows.firstRowReady || !Providers.hasAccounts || (homeLibraries.length === 0 &&
                                                                                          !Home.loading)
    property bool providerChooserOpen: false
    readonly property bool modalVisible: providerChooserOpen
    property int libraryRevision: 0
    // What the provider button says Home is showing right now.
    readonly property string providerSummary: {
        const choices = Home.providerChoices
        const shown = choices.filter(choice => choice.shown)
        if (!Home.providerFilterActive || shown.length === choices.length)
            return "All providers"
        return shown.length === 1 ? String(shown[0].name) : shown.length + " of " + choices.length + " providers"
    }
    readonly property var homeLibraries: {
        // Read the signals' properties so scoped identity/visibility changes
        // rebuild the array without mutating the global Libraries model.
        const hiddenProviders = Home.hiddenProviderIds
        const filterActive = Home.providerFilterActive
        const revision = libraryRevision
        const accounts = Providers.accounts
        const hidden = Libraries.hiddenLibraries
        const result = []
        for (let index = 0; index < Libraries.count; ++index) {
            const library = Libraries.get(index)
            if (Home.includesItem(String(library.libraryId || "")))
                result.push(library)
        }
        return result
    }
    readonly property var homeHiddenLibraries: {
        const hiddenProviders = Home.hiddenProviderIds
        const filterActive = Home.providerFilterActive
        const accounts = Providers.accounts
        return Libraries.hiddenLibraries.filter(library => Home.includesItem(String(library.libraryId || "")))
    }

    function libraryIndex(id) {
        for (let index = 0; index < Libraries.count; ++index) {
            if (String(Libraries.get(index).libraryId) === String(id))
                return index
        }
        return -1
    }

    function moveHomeLibrary(from, to) {
        const source = homeLibraries[from]
        const destination = homeLibraries[to]
        return source && destination ? Libraries.moveLibrary(libraryIndex(source.libraryId), libraryIndex(
                                                                 destination.libraryId)) : false
    }

    function closeProviderChooser() {
        providerChooserOpen = false
        InputKeys.focus(providerButton)
    }

    function openProviderChooser() {
        providerChooserOpen = true
    }

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
        const hidden = homeHiddenLibraries
        const sections = [
                  {
                      "key": "libraries",
                      "title": "Libraries",
                      "model": homeLibraries,
                      "kind": "library",
                      "moveItem": (from, to) => root.moveHomeLibrary(from, to),
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
                          "reserveWhileLoading": true
                      }, {
                          "key": "nextUpItems",
                          "title": "Next Up",
                          "model": Home.nextUpItems,
                          "kind": "landscape",
                          "reserveWhileLoading": (Home.latestLibraryRows || []).some(row => row.collectionType
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
                              "reserveWhileLoading": true,
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
            if (!App.openLibraryById(String(item.libraryId || "")))
                return
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
        if (providerChooserOpen) {
            if (InputKeys.isBack(key, false, false))
                return false
            return providerPicker.item ? providerPicker.item.routeKey(key, phase, repeat) : true
        }
        if (providerButton.activeFocus || allProvidersButton.activeFocus) {
            if (phase === "press" && key === Qt.Key_Down)
                return rows.focusPreferred("libraries")
            if (phase === "press" && key === Qt.Key_Up) {
                if (root.shell)
                    root.shell.focusNavBar()
                return true
            }
            if (phase === "press" && (key === Qt.Key_Left || key === Qt.Key_Right) && allProvidersButton.visible) {
                InputKeys.focus(providerButton.activeFocus ? allProvidersButton : providerButton)
                return true
            }
            return InputKeys.isDirection(key) || InputKeys.isAccept(key)
        }
        if (phase === "press" && InputKeys.isDirection(key) && (!rows.activeFocus || (homeLibraries.length === 0
                                                                                      && Home.resumeItems.count === 0
                                                                                      && Home.nextUpItems.count === 0
                                                                                      && Home.latestLibraryRows.length
                                                                                      === 0))) {
            InputKeys.focus(providerButton)
            return true
        }
        return rows.routeKey(key, phase, repeat)
    }

    function activate() {
        if (providerChooserOpen) {
            if (providerPicker.item)
                providerPicker.item.activate()
            return
        }
        if (providerButton.activeFocus) {
            openProviderChooser()
            return
        }
        if (allProvidersButton.activeFocus) {
            Home.showAllProviders()
            return
        }
        if (!rows.activeFocus) {
            InputKeys.focus(providerButton)
            return
        }
        rows.activate()
    }

    function back() {
        if (!providerChooserOpen)
            return false
        closeProviderChooser()
        return true
    }

    function longPress() {
        return !providerChooserOpen && rows.activeFocus && rows.longPress()
    }

    function currentMediaItem() {
        return !providerChooserOpen && rows.activeFocus ? rows.currentItem() : ({})
    }
    Connections {
        target: Libraries
        function onModelReset() {
            root.libraryRevision += 1
        }
        function onRowsMoved() {
            root.libraryRevision += 1
        }
    }

    Component.onCompleted: rows.reset()

    Connections {
        target: Home
        function onLatestLibraryRowsChanged() {
            Qt.callLater(rows.repair)
        }
        function onProviderScopeChanged() {
            Qt.callLater(rows.repair)
        }
    }

    ColumnLayout {
        id: homeHeader
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Metrics.pageMarginPx
        spacing: Metrics.scaled(8)

        RowLayout {
            Layout.fillWidth: true
            AppText {
                text: "Home"
                font.pixelSize: Metrics.titleSizePx
                Layout.fillWidth: true
            }
            ActionButton {
                id: providerButton
                objectName: "homeProviderChooserButton"
                iconName: "filter_list"
                text: root.providerSummary
                Accessible.name: "Providers on Home: " + root.providerSummary
                Accessible.description: "Turn providers on or off for Home only. Search and playback remain unchanged."
                onClicked: root.openProviderChooser()
            }
            ActionButton {
                id: allProvidersButton
                visible: Home.providerScopeMessage.length > 0
                text: "Show all"
                onClicked: Home.showAllProviders()
            }
        }
        SecondaryText {
            Layout.fillWidth: true
            visible: Home.providerScopeMessage.length > 0
            text: Home.providerScopeMessage
            wrapMode: Text.WordWrap
            maximumLineCount: 4
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
    }

    RowStackView {
        id: rows

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: homeHeader.bottom
        anchors.bottom: parent.bottom
        anchors.leftMargin: Metrics.pageMarginPx
        anchors.rightMargin: Metrics.pageMarginPx
        anchors.topMargin: Metrics.scaled(8)
        shell: root.shell
        sections: root.buildSections()
        loading: Home.loading
        contextReturnRoute: "home"
        measureFirstRow: true
        focus: true

        onEdgeUp: InputKeys.focus(providerButton)
        onActivated: (section, index, item) => root.activateAt(section, index, item)
        onFirstRowReadyChanged: if (firstRowReady)
                                    InputLatency.mark(root.uiTransitionToken, "first_delegate")
    }

    Loader {
        id: providerPicker
        anchors.fill: parent
        active: root.providerChooserOpen
        z: 200
        sourceComponent: HomeProviderPicker {
            anchorItem: providerButton
            onDismissed: root.closeProviderChooser()
        }
    }
}
