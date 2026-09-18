pragma ComponentBehavior: Bound

import QtQuick
import "../theme"
import "../primitives"

FocusScope {
    id: root

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
    signal closed
    readonly property int windowWidth: root.Window.window ? root.Window.window.width : 1920
    readonly property int menuEdgeMargin: Math.max(12, Metrics.gapPx)
    readonly property int menuRowHeight: Math.max(Metrics.touchTargetPx, Metrics.scaled(36))
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
    // Management only exists on a source that has playlists and collections,
    // so it is only consulted behind the capability.
    readonly property var management: ProviderCapabilities.libraryManagement ? Management : null
    readonly property bool canManagePlaylists: management ? management.currentUserCanManagePlaylists : false
    readonly property bool canManageCollections: management ? management.currentUserCanManageCollections : false
    readonly property bool canRenameItem: management ? management.currentUserCanRenameItems : false
    readonly property bool canDeleteItem: management ? management.currentUserCanDeleteItems : false

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
            if (canManagePlaylists && queueable)
                options.push({
                                 action: "playlist",
                                 icon: "playlist_add",
                                 label: "Add to playlist",
                                 checked: false
                             })
            if (canManageCollections && collectionEligible)
                options.push({
                                 action: "collection",
                                 icon: "library_add",
                                 label: "Add to collection",
                                 checked: false
                             })
            if (inPlaylist && item.playlistItemId && management) {
                options.push({
                                 action: "moveUp",
                                 icon: "keyboard_arrow_up",
                                 label: "Move up",
                                 checked: false
                             })
                options.push({
                                 action: "moveDown",
                                 icon: "keyboard_arrow_down",
                                 label: "Move down",
                                 checked: false
                             })
                options.push({
                                 action: "removeParent",
                                 icon: "remove_circle",
                                 label: "Remove from playlist",
                                 checked: false
                             })
            } else if (inCollection && canManageCollections) {
                options.push({
                                 action: "removeParent",
                                 icon: "remove_circle",
                                 label: "Remove from collection",
                                 checked: false
                             })
            }
            if ((itemType === "Playlist" && canManagePlaylists) || canRenameItem)
                options.push({
                                 action: "rename",
                                 icon: "drive_file_rename_outline",
                                 label: "Rename",
                                 checked: false
                             })
            if (canDeleteItem)
                options.push({
                                 action: "delete",
                                 icon: "delete",
                                 label: "Delete",
                                 checked: false
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
        item = ({})
        anchorItem = null
        context = ({})
        menuOptions = []
        closed()
    }

    function activateMenuIndex(index) {
        if (index < 0 || index >= menuOptions.length)
            return
        const action = menuOptions[index].action
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
        } else if (action === "playlist" || action === "collection") {
            shell.openManagement(action, item)
        } else if (action === "removeParent") {
            shell.openManagement("remove", item)
        } else if (action === "moveUp" || action === "moveDown") {
            management.movePlaylistItemInCurrent(item, action === "moveUp" ? -1 : 1)
        } else if (action === "rename" || action === "delete") {
            shell.openManagement(action, item)
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
        width: root.menuPanelWidth
        open: root.opened
        openHeight: root.menuPanelHeight
        baseColor: Theme.floatingPanel

        MenuListView {
            id: menuList
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
                highlighted: ListView.isCurrentItem
                rowHeight: root.menuRowHeight
                compact: true
                onHovered: menuList.currentIndex = index
                onActivated: root.activateMenuIndex(index)
            }
        }
    }
}
