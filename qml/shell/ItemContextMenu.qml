pragma ComponentBehavior: Bound

import QtQuick
import "../theme"
import "../primitives"

FocusScope {
    id: root
    objectName: libraryContext ? "libraryContextMenu" : "itemContextMenu"

    property var item: ({})
    property var anchorItem: null
    property var context: ({})
    property var shell
    property int menuIndex: 0
    property var menuOptions: []
    property bool favoriteState: Boolean(item && item.favorite)
    property bool playedState: Boolean(item && item.played)
    property bool opened: false
    property bool backdropDismissArmed: false
    property int actionsRequest: -1
    property var providerActions: []
    property bool actionsLoading: false
    property string actionsProblem: ""
    readonly property string containerId: String(context && context.containerId || "")
    readonly property bool libraryContext: Boolean(context.library)
    property bool showingHiddenLibraries: false
    readonly property string entryId: String(context && context.entryId || item && (item.playlistItemId || item.entryId)
                                             || "")
    readonly property string editorContainerId: itemType === "Playlist" || itemType === "BoxSet" ? itemId : containerId
    signal closed
    readonly property int windowWidth: root.Window.window ? root.Window.window.width : 1920
    readonly property int menuEdgeMargin: Math.max(12, Metrics.gapPx)
    readonly property int menuRowHeight: Math.max(Metrics.touchTargetPx, Metrics.scaled(menuOptions.some(option => Boolean(
                                                                                                                       option.reason))
                                                                                        ? 54 : 36))
    readonly property int menuPanelWidth: Math.min(windowWidth - menuEdgeMargin * 2, Math.max(280, Math.min(320,
                                                                                                            Math.round(
                                                                                                                windowWidth
                                                                                                                * 0.18))))
    readonly property int menuPanelHeight: menuOptions.length <= 0 ? 0 : Math.min(Math.max(0, height - menuEdgeMargin
                                                                                           * 2), menuOptions.length
                                                                                  * menuRowHeight + 12)
    readonly property string itemId: item && item.movieId ? String(item.movieId) : ""
    readonly property string itemType: item && item.itemType ? String(item.itemType) : ""
    readonly property bool episodeOrSeason: itemType === "Episode" || itemType === "Season"
    readonly property bool hasProgress: Number(item && item.resumeTicks ? item.resumeTicks : 0) > 0
    readonly property bool continueWatchingContext: String(context && context.source ? context.source : "")
                                                    === "resumeItems"
    readonly property bool partialEpisode: itemType === "Episode" && hasProgress && !playedState
    readonly property bool actionable: itemId.length > 0
    readonly property bool queueable: actionable && item && item.playable !== false
    readonly property string currentViewKind: String(Browse.viewKind || "")
    readonly property bool inPlaylist: currentViewKind === "playlist"
    readonly property bool inCollection: currentViewKind === "boxset" || currentViewKind === "collection"
    readonly property bool collectionEligible: actionable && (itemType === "Movie" || itemType === "Series" || itemType
                                                              === "Episode")

    visible: opened
    focus: opened

    Connections {
        target: ItemState
        enabled: root.opened
        function onFavoriteChanged(changedItemId, favorite) {
            if (root.itemId === changedItemId)
                root.favoriteState = favorite
        }
        function onPlayedChanged(changedItemId, played) {
            if (root.itemId === changedItemId)
                root.playedState = played
        }
    }

    Connections {
        target: Sources
        function onItemActionsReady(requestId, actions, problem) {
            if (!root.opened || requestId !== root.actionsRequest)
                return
            const selected = root.menuOptions[root.menuIndex]
            root.providerActions = actions
            root.actionsLoading = false
            root.actionsProblem = problem
            root.rebuildMenu()
            const index = selected ? root.menuOptions.findIndex(option => option.action === selected.action) : -1
            root.menuIndex = Math.max(0, index)
            Qt.callLater(root.positionMenu)
        }
        function onExtensionSupportChanged(accountId) {
            if (root.opened && !root.libraryContext)
                root.loadProviderActions()
        }
    }

    Connections {
        target: Libraries
        enabled: root.opened && root.libraryContext
        function onHiddenLibrariesChanged() {
            root.rebuildMenu()
            root.menuIndex = Math.max(0, Math.min(root.menuIndex, root.menuOptions.length - 1))
            Qt.callLater(root.positionMenu)
        }
        function onModelReset() {
            if (!root.showingHiddenLibraries)
                root.closeMenu()
        }
    }

    function loadProviderActions() {
        providerActions = []
        actionsProblem = ""
        actionsLoading = true
        actionsRequest = Sources.requestItemActions(itemId, itemType, containerId, entryId)
        rebuildMenu()
    }

    function clamp(value, minimum, maximum) {
        return Math.max(minimum, Math.min(maximum, value))
    }

    function syncItemState() {
        favoriteState = Boolean(item && item.favorite)
        playedState = Boolean(item && item.played)
    }

    function seasonTitle() {
        return item && item.seasonNumber > 0 ? "Season " + item.seasonNumber : "Season"
    }

    function rebuildMenu() {
        const options = []
        if (libraryContext) {
            if (showingHiddenLibraries) {
                for (const library of Libraries.hiddenLibraries) {
                    const origin = Sources.originOf(String(library.libraryId))
                    options.push({
                                     action: "unhide",
                                     libraryId: String(library.libraryId),
                                     icon: "visibility",
                                     label: "Show " + String(library.name || "library"),
                                     reason: Sources.multipleSources && origin ? String(origin.serverName
                                                                                        || origin.providerName || "") :
                                                                                 ""
                                 })
                }
                if (options.length === 0)
                    options.push({
                                     action: "empty",
                                     icon: "check",
                                     label: "No hidden libraries",
                                     enabled: false
                                 })
                options.push({
                                 action: "done",
                                 icon: "check",
                                 label: "Done"
                             })
            } else {
                if (context.row)
                    options.push({
                                     action: "moveLibrary",
                                     icon: "swap_horiz",
                                     label: "Move"
                                 })
                options.push({
                                 action: "hideLibrary",
                                 icon: "visibility_off",
                                 label: "Hide library"
                             })
                options.push({
                                 action: "hiddenLibraries",
                                 icon: "visibility",
                                 label: "Show hidden libraries"
                             })
            }
            menuOptions = options
            return true
        }
        if (continueWatchingContext && actionable)
            options.push({
                             action: "details",
                             icon: "description",
                             label: "Go to details",
                             checked: false
                         })
        if (episodeOrSeason && item.seriesId)
            options.push({
                             action: "series",
                             icon: "live_tv",
                             label: "Go to series",
                             checked: false
                         })
        if ((itemType === "Episode" && item.seriesId && item.seasonId) || (itemType === "Season" && item.seriesId
                                                                           && itemId))
            options.push({
                             action: "season",
                             icon: "video_library",
                             label: "Go to season",
                             checked: false
                         })
        if (actionable) {
            if (queueable) {
                options.push({
                                 action: "playNext",
                                 icon: "playlist_play",
                                 label: "Play next",
                                 checked: false
                             })
                options.push({
                                 action: "addQueue",
                                 icon: "queue_music",
                                 label: "Add to queue",
                                 checked: false
                             })
            }
            if (continueWatchingContext)
                options.push({
                                 action: "unwatched",
                                 icon: "visibility_off",
                                 label: "Mark unwatched",
                                 checked: false
                             })
            else {
                options.push({
                                 action: "played",
                                 icon: playedState ? "visibility" : "visibility_off",
                                 label: playedState ? "Mark unwatched" : "Mark watched",
                                 checked: playedState
                             })
                if (partialEpisode)
                    options.push({
                                     action: "clear",
                                     icon: "replay",
                                     label: "Clear progress",
                                     checked: false
                                 })
            }
            options.push({
                             action: "favorite",
                             icon: favoriteState ? "favorite" : "favorite_border",
                             label: favoriteState ? "Remove favourite" : "Add favourite",
                             checked: favoriteState
                         })
            if (Downloads.supported && queueable && (Downloads.statusFor(itemId).id || Sources.downloadOptions(
                                                         itemId).length > 0))
                options.push({
                                 action: "download",
                                 icon: "download",
                                 label: "Download…",
                                 checked: false
                             })
            if (editorContainerId && Sources.collectionEditingAvailable(editorContainerId))
                options.push({
                                 action: "collectionEditor",
                                 icon: "edit",
                                 label: "Manage entries",
                                 checked: false
                             })
            for (const action of providerActions)
                options.push({
                                 action: "provider:" + action.id,
                                 icon: action.icon || "more_horiz",
                                 label: action.label,
                                 enabled: action.enabled !== false,
                                 reason: action.reason || "",
                                 checked: false
                             })
            if (actionsLoading || actionsProblem)
                options.push({
                                 action: "providerStatus",
                                 icon: "more_horiz",
                                 label: actionsLoading ? "Loading provider actions…" : actionsProblem,
                                 enabled: false
                             })
        }
        if (itemType !== "Series" && itemType !== "Season" && item && (item.movieId || item.id || item.title || item.displayTitle
                                                                       || item.seriesName))
            options.push({
                             action: "info",
                             icon: "info",
                             label: "Media info",
                             checked: false
                         })
        menuOptions = options
        return menuOptions.length > 0
    }

    function positionMenu() {
        const edge = menuEdgeMargin
        let desiredX = Math.round((width - menuPanel.width) / 2)
        let desiredY = Math.round((height - menuPanel.height) / 2)
        if (anchorItem) {
            const anchor = anchorItem.mapToItem(root, 0, 0)
            desiredX = anchor.x + anchorItem.width - menuPanel.width
            const below = anchor.y + Math.min(anchorItem.height, menuRowHeight) + 8
            desiredY = below + menuPanel.height <= height - edge ? below : anchor.y - menuPanel.height - 8
        }
        menuPanel.x = clamp(desiredX, edge, Math.max(edge, width - menuPanel.width - edge))
        menuPanel.y = clamp(desiredY, edge, Math.max(edge, height - menuPanel.height - edge))
    }

    function openForItem(nextItem, anchor, nextContext) {
        item = nextItem || ({})
        anchorItem = anchor || null
        context = nextContext || ({})
        showingHiddenLibraries = Boolean(context.showHidden)
        providerActions = []
        actionsProblem = ""
        actionsLoading = true
        syncItemState()
        if (!rebuildMenu())
            return false
        menuIndex = 0
        backdropArmTimer.stop()
        backdropDismissArmed = !Boolean(context.deferBackdropDismissal)
        if (!backdropDismissArmed) {
            backdropArmTimer.interval = 450
            backdropArmTimer.restart()
        }
        opened = true
        if (!libraryContext)
            loadProviderActions()
        InputKeys.focus(menuList)
        Qt.callLater(positionMenu)
        return true
    }

    function finishOpeningGesture() {
        if (opened && !backdropDismissArmed) {
            backdropArmTimer.interval = 150
            backdropArmTimer.restart()
        }
    }

    function closeMenu() {
        if (!opened)
            return
        backdropArmTimer.stop()
        backdropDismissArmed = false
        opened = false
        if (!libraryContext)
            Sources.cancelItemActions()
        actionsRequest = -1
        providerActions = []
        item = ({})
        anchorItem = null
        context = ({})
        menuOptions = []
        closed()
    }

    function activateMenuIndex(index) {
        if (index < 0 || index >= menuOptions.length)
            return
        if (menuOptions[index].enabled === false)
            return
        const action = menuOptions[index].action
        if (libraryContext) {
            const libraryId = String(item.libraryId || "")
            if (action === "hiddenLibraries") {
                showingHiddenLibraries = true
                rebuildMenu()
                menuIndex = 0
                Qt.callLater(positionMenu)
                return
            }
            if (action === "unhide") {
                Libraries.showLibrary(String(menuOptions[index].libraryId))
                return
            }
            if (action === "moveLibrary") {
                const row = context.row
                closeMenu()
                Qt.callLater(() => {
                    if (row)
                        row.beginMoveById(libraryId)
                })
                return
            }
            if (action === "hideLibrary")
                Libraries.hideLibrary(libraryId)
            closeMenu()
            return
        }
        if (action === "details") {
            shell.openDetailsAt(context.model, Number(context.index || 0), "resume", String(context.returnRoute
                                                                                            || "home"))
        } else if (action === "series") {
            const seriesId = String(item.seriesId || "")
            if (seriesId.length > 0)
                shell.openSeriesDetails(seriesId, String(item.seriesName || ""), String(context.returnRoute || "home"))
        } else if (action === "season") {
            const seasonId = itemType === "Season" ? itemId : String(item.seasonId || "")
            if (item.seriesId && seasonId)
                shell.openSeasonDetails(String(item.seriesId), seasonId, itemType === "Season" ? String(item.title
                                                                                                        || seasonTitle(
                                                                                                            )) : seasonTitle(
                                                                                                     ), String(
                                            item.seriesName || ""), String(context.returnRoute || "home"))
        } else if (action === "playNext") {
            App.playNextFromItem(item)
        } else if (action === "addQueue") {
            App.addToQueueFromItem(item)
        } else if (action === "played") {
            playedState = !playedState
            ItemState.setPlayed(itemId, playedState)
        } else if (action === "unwatched") {
            playedState = false
            ItemState.clearProgress(itemId)
        } else if (action === "clear") {
            playedState = false
            ItemState.clearProgress(itemId)
        } else if (action === "favorite") {
            favoriteState = !favoriteState
            ItemState.setFavorite(itemId, favoriteState)
        } else if (action === "download") {
            shell.openDownloads(itemId, anchorItem)
        } else if (action === "collectionEditor") {
            shell.openCollectionEditor(editorContainerId, itemType === "Playlist" || itemType === "BoxSet" ? String(
                                                                                                                 item.title
                                                                                                                 || "") : String(
                                                                                                                 context.containerTitle
                                                                                                                 || Browse.title
                                                                                                                 || ""))
        } else if (action.startsWith("provider:")) {
            Sources.runItemAction(action.slice(9), itemId, itemType, containerId, entryId)
        } else if (action === "info") {
            shell.openMediaInfo(item)
        }
        closeMenu()
    }

    function routeKey(key, phase, repeat) {
        return opened && menuList.routeKey(key, phase, repeat)
    }

    function activate() {
        if (opened)
            menuList.activate()
    }

    function back() {
        if (!opened)
            return false
        closeMenu()
        return true
    }

    MouseArea {
        anchors.fill: parent
        onClicked: if (root.backdropDismissArmed)
                       root.closeMenu()
    }

    Timer {
        id: backdropArmTimer
        repeat: false
        onTriggered: if (root.opened)
                         root.backdropDismissArmed = true
    }

    PopupMenuPanel {
        id: menuPanel
        objectName: root.libraryContext ? "libraryContextMenuPanel" : "itemContextMenuPanel"
        width: root.menuPanelWidth
        open: root.opened
        openHeight: root.menuPanelHeight
        baseColor: Theme.floatingPanel

        MenuListView {
            id: menuList
            objectName: "contextMenuList"
            anchors.fill: parent
            anchors.margins: 6
            model: root.menuOptions
            currentIndex: root.menuIndex
            onCurrentIndexChanged: root.menuIndex = currentIndex
            onDismissed: root.closeMenu()
            onAccepted: index => root.activateMenuIndex(index)

            delegate: MenuRow {
                required property int index
                required property var modelData
                width: menuList.width
                label: modelData.label || ""
                iconName: modelData.icon || "more_horiz"
                checked: Boolean(modelData.checked)
                actionable: modelData.enabled !== false
                detail: modelData.reason || ""
                highlighted: ListView.isCurrentItem
                rowHeight: root.menuRowHeight
                compact: true
                onHovered: menuList.currentIndex = index
                onActivated: root.activateMenuIndex(index)
            }
        }
    }
}
