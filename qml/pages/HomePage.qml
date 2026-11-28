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
    readonly property bool pendingContent: !Providers.restored || Home.loading || Providers.accounts.some(account
                                                                                                          => account.enabled
                                                                                                             && !account.removing
                                                                                                             && !account.needsSignIn
                                                                                                             && account.connectionState
                                                                                                             === "starting")
    readonly property bool hasHomeContent: homeLibraries.length > 0 || Home.resumeItems.count > 0
                                           || Home.nextUpItems.count > 0 || Home.latestLibraryRows.some(row
                                                                                                        => row.model
                                                                                                           && row.model.count
                                                                                                           > 0)
    readonly property bool initialLoading: pendingContent && !hasHomeContent
    readonly property string connectionWarning: Providers.accounts.filter(account => account.enabled && (
                                                                                         account.needsSignIn
                                                                                         || account.connectionState
                                                                                         === "failed")).map(account
                                                                                                            => String(
                                                                                                                   account.detail
                                                                                                                   || account.providerName)
                                                                                                               + ": " + String(
                                                                                                                   account.errorText
                                                                                                                   || (account.needsSignIn
                                                                                                                       ? "Sign in again to use this profile." :
                                                                                                                         "Couldn't reach this server."))).join(
                                                    "\n")
    // An all-hidden library row has management controls instead of cards.
    readonly property bool contentReady: initialLoading || rows.firstRowReady || !Providers.hasAccounts || (
                                             homeLibraries.length === 0 && !pendingContent)
    property int libraryRevision: 0
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
                              "key": "latestLibrary:" + String(row.libraryId),
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

    // Controls above the rows remain reachable even while the first load is pending.
    function edgeUp() {
        if (accountsButton.visible)
            InputKeys.focus(accountsButton)
        else if (allProvidersButton.visible)
            InputKeys.focus(allProvidersButton)
        else if (root.shell)
            root.shell.focusNavBar()
    }

    function routeKey(key, phase, repeat) {
        if (allProvidersButton.activeFocus || accountsButton.activeFocus) {
            // Show all sits above the warning's button; move between them first.
            const both = allProvidersButton.visible && accountsButton.visible
            if (phase === "press" && both && key === Qt.Key_Down && allProvidersButton.activeFocus) {
                InputKeys.focus(accountsButton)
                return true
            }
            if (phase === "press" && both && key === Qt.Key_Up && accountsButton.activeFocus) {
                InputKeys.focus(allProvidersButton)
                return true
            }
            if (phase === "press" && key === Qt.Key_Down)
                return initialLoading || rows.focusPreferred("libraries")
            if (phase === "press" && key === Qt.Key_Up) {
                if (root.shell)
                    root.shell.focusNavBar()
                return true
            }
            return InputKeys.isDirection(key) || InputKeys.isAccept(key)
        }
        if (initialLoading && InputKeys.isDirection(key)) {
            if (phase === "press") {
                if (key === Qt.Key_Up && root.shell)
                    root.shell.focusNavBar()
                else if (key === Qt.Key_Down)
                    edgeUp()
            }
            return true
        }
        if (phase === "press" && InputKeys.isDirection(key) && (!rows.activeFocus || (homeLibraries.length === 0
                                                                                      && Home.resumeItems.count === 0
                                                                                      && Home.nextUpItems.count === 0
                                                                                      && Home.latestLibraryRows.length
                                                                                      === 0))) {
            edgeUp()
            return true
        }
        return rows.routeKey(key, phase, repeat)
    }

    function activate() {
        if (accountsButton.activeFocus) {
            if (shell)
                shell.pushRoute("accounts")
            return
        }
        if (allProvidersButton.activeFocus) {
            Home.showAllProviders()
            return
        }
        if (initialLoading)
            return
        if (!rows.activeFocus) {
            edgeUp()
            return
        }
        rows.activate()
    }

    function longPress() {
        return rows.activeFocus && rows.longPress()
    }

    function currentMediaItem() {
        return rows.activeFocus ? rows.currentItem() : ({})
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
            RowLayout {
                visible: root.pendingContent && root.hasHomeContent
                spacing: Metrics.scaled(8)
                BusySpinner {
                    Layout.preferredWidth: Metrics.scaled(18)
                    Layout.preferredHeight: width
                    running: parent.visible
                    color: Theme.textMuted
                }
                SecondaryText {
                    text: "Loading more…"
                    color: Theme.textMuted
                    font.pixelSize: Metrics.metaSizePx
                }
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
        RowLayout {
            Layout.fillWidth: true
            visible: root.connectionWarning.length > 0
            spacing: Metrics.gapPx
            SecondaryText {
                Layout.fillWidth: true
                text: root.connectionWarning
                color: Theme.errorText
                wrapMode: Text.WordWrap
            }
            ActionButton {
                id: accountsButton
                text: "Profiles & servers"
                onClicked: if (root.shell)
                               root.shell.pushRoute("accounts")
            }
        }
    }

    Column {
        anchors.centerIn: parent
        visible: root.initialLoading
        spacing: Metrics.scaled(16)
        BusySpinner {
            anchors.horizontalCenter: parent.horizontalCenter
            width: Metrics.scaled(40)
            height: width
            running: root.initialLoading
            color: Theme.textMuted
        }
        SecondaryText {
            text: "Loading your libraries…"
            color: Theme.textMuted
            font.pixelSize: Metrics.bodySizePx
        }
    }

    RowStackView {
        id: rows

        visible: !root.initialLoading
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

        onEdgeUp: root.edgeUp()
        onActivated: (section, index, item) => root.activateAt(section, index, item)
        onFirstRowReadyChanged: if (firstRowReady)
                                    InputLatency.mark(root.uiTransitionToken, "first_delegate")
    }
}
