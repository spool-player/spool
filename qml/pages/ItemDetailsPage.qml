pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"
import "../shell/RoutePolicy.js" as RoutePolicy

FocusScope {
    id: root

    property var shell
    readonly property var contextRow: detailRowsLoader.item ? detailRowsLoader.item.contextRow : null
    readonly property var peopleRow: detailRowsLoader.item ? detailRowsLoader.item.peopleRow : null
    readonly property var similarRow: detailRowsLoader.item ? detailRowsLoader.item.similarRow : null
    readonly property var seasonPickerList: seasonPickerPanel.menuList
    readonly property var routeContext: RoutePolicy.detailsContext(shell ? shell.routeArgs : ({}), Browse.items)
    readonly property var itemModel: routeContext.model
    readonly property int selectedIndex: routeContext.index
    readonly property var routeItem: routeContext.item
    readonly property var fullDetailItem: Content.detailItem && String(Content.detailItem.movieId || "") === String(
                                              routeItem.movieId || "") ? Content.detailItem : ({})
    readonly property var item: fullDetailItem.movieId ? fullDetailItem : routeItem
    readonly property string detailsReturnRoute: routeContext.returnRoute
    readonly property bool routeActive: Boolean(shell && shell.route === "itemDetails")
    readonly property string typeText: item.itemType || "Media"
    readonly property string titleText: typeText === "Episode" && item.title ? item.title : (item.displayTitle
                                                                                             || item.title
                                                                                             || item.seriesName
                                                                                             || "Selected item")
    readonly property string seriesTitle: item.seriesName || (typeText === "Series" ? titleText : "")
    readonly property string seasonTitleText: seasonTitle()
    readonly property string seriesIdText: item.seriesId || ""
    readonly property string seasonIdText: typeText === "Season" ? String(item.movieId || "") : String(item.seasonId
                                                                                                       || "")

    readonly property var externalLinks: {
        const links = []
        const source = !Platform.isTV && item.externalUrls ? item.externalUrls : []
        for (let index = 0; index < source.length; ++index) {
            const name = String(source[index].name || "").trim()
            const url = String(source[index].url || "").trim()
            if (name.length > 0 && url.toLowerCase().indexOf("https://") === 0)
            links.push({
                           "name": name,
                           "url": url
                       })
        }
        return links
    }
    readonly property string letterboxdUrl: {
        const tmdbId = String(item.tmdbId || "").trim()
        if (tmdbId.length > 0)
        return "https://letterboxd.com/tmdb/" + encodeURIComponent(tmdbId) + "/"
        const imdbId = String(item.imdbId || "").trim()
        return imdbId.length > 0 ? "https://letterboxd.com/imdb/" + encodeURIComponent(imdbId) + "/" : ""
    }
    readonly property bool showExternalActions: externalLinks.length > 0
    readonly property bool showLetterboxdAction: !Platform.isTV && typeText === "Movie" && letterboxdUrl.length > 0

    readonly property bool canPlay: Boolean(item && item.playable)
    readonly property bool albumDetail: typeText === "MusicAlbum"
    readonly property bool canPlayEpisodicContainer: typeText === "Series" ? String(item.movieId || "").length > 0 : typeText === "Season"
                                                                             && seriesIdText.length > 0
                                                                             && seasonIdText.length > 0
    readonly property bool canPlayAlbum: albumDetail && contextCount > 0
    readonly property bool showPrimaryAction: canPlayEpisodicContainer || canPlayAlbum || (selectedIndex >= 0
                                                                                           && canPlay)

    readonly property bool hasProgress: Number(item.resumeTicks || 0) > 0 && Number(item.runtimeTicks || 0) > 0
    readonly property string lane: Metrics.lane(width)
    readonly property int detailTitlePx: Math.min(Metrics.scaled(68), Metrics.titleSizePx + Metrics.scaled(24))
    readonly property int contentMargin: Metrics.pageMarginPx
    readonly property int rowPosterWidth: Metrics.detailRowPosterWidth()
    readonly property int rowLandscapeWidth: Math.round(rowPosterWidth * 1.75)
    readonly property int rowGap: Math.max(14, Metrics.gapPx)
    readonly property bool compactEpisodicDetail: typeText === "Season" || typeText === "Episode"
    readonly property bool smallZoom: Metrics.uiScalePercent <= 100
    readonly property string backgroundArt: Art.url(item, "backdrop", Math.ceil(width))
    readonly property int sideArtWidth: Math.min(Math.round(width * (albumDetail ? 0.26 : 0.38)), Metrics.scaled(albumDetail
                                                                                                                 ? 620 : 1100))
    readonly property string stillArt: Art.url(item, albumDetail ? "poster" : "landscape")
    // Side art is a second column, so it waits for a lane wide enough to
    // hold one without squeezing the copy beside it.
    readonly property bool showSideArt: lane === "wide" && stillArt.length > 0
    readonly property var technicalInfo: {
        // Anything with a playable file has a readout worth showing, which is
        // the same rule the media-info overlay uses. Series and Season are the
        // only kinds that are containers rather than something with streams.
        if (!fullDetailItem.movieId || !mediaInfoAvailable)
        return null
        const smart = String(Settings.values["audio/trackMode"] || "Default") === "Smart"
        return Content.detailMediaInfo(smart ? String(Settings.values["subtitles/language"] || "") : "")
    }
    readonly property real copyWidth: showSideArt ? Math.min(width * 0.56, Metrics.scaled(940)) : width - contentMargin
                                                    * 2
    readonly property bool loadingDetailRows: Content.detailRowsBusy
    readonly property int contextCount: Content.detailSeasons ? Content.detailSeasons.count : 0
    readonly property int seasonOptionCount: Content.detailSeasonOptions ? Content.detailSeasonOptions.count : 0
    readonly property int similarCount: Content.detailSimilarItems ? Content.detailSimilarItems.count : 0
    readonly property bool contextPosterCards: typeText === "Series" || typeText === "BoxSet"
    readonly property bool contextItemsPossible: albumDetail || contextPosterCards || ((typeText === "Episode"
                                                                                        || typeText === "Season")
                                                                                       && seriesIdText.length > 0)
    readonly property bool reserveContextRow: contextItemsPossible && contextCount === 0 && loadingDetailRows
    readonly property bool showContextPlaybackActions: contextCount > 0 && typeText !== "Series"
    readonly property bool mediaInfoAvailable: typeText !== "Series" && typeText !== "Season"
    readonly property bool showContextRow: contextCount > 0 || reserveContextRow
    readonly property bool reserveSimilarRow: loadingDetailRows && (typeText === "Movie" || typeText === "Series" || typeText
                                                                    === "Episode")
    readonly property bool showSimilarRow: similarCount > 0 || reserveSimilarRow
    readonly property var metadataPeople: fullDetailItem.people && fullDetailItem.people.length > 0
                                          ? fullDetailItem.people : (item.people || [])
    readonly property var people: metadataPeople
    readonly property bool reservePeopleRow: loadingDetailRows && !fullDetailItem.movieId && (typeText === "Movie"
                                                                                              || typeText === "Series"
                                                                                              || typeText === "Episode"
                                                                                              || typeText === "Season")
    readonly property bool showPeopleRow: people.length > 0 || reservePeopleRow
    readonly property var genreList: fullDetailItem.genres && fullDetailItem.genres.length > 0 ? fullDetailItem.genres :
                                                                                                 (item.genres || [])
    readonly property var studioList: fullDetailItem.studios && fullDetailItem.studios.length > 0
                                      ? fullDetailItem.studios : (item.studios || [])
    readonly property var metadataRows: buildMetadataRows()
    readonly property bool showMetadataPanel: metadataRows.length > 0
    readonly property bool showSeriesLink: (typeText === "Episode" || typeText === "Season") && seriesIdText.length > 0
                                           && seriesTitle.length > 0
    readonly property bool showSeasonLink: compactEpisodicDetail && seriesIdText.length > 0 && (currentSeasonId.length
                                                                                                > 0 || Number(
                                                                                                    item.seasonNumber
                                                                                                    || 0) > 0)
    readonly property string currentSeasonId: typeText === "Season" ? String(item.movieId || "") : seasonIdText

    property bool favoriteState: false
    property bool playedState: false
    property bool overflowOpen: false
    property point overflowAnchorPoint: Qt.point(0, 0)
    property string focusZone: "actions"
    property int actionIndex: 0
    property int overflowIndex: 0
    property string loadedDetailKey: ""
    property bool seasonPickerOpen: false
    property int seasonPickerIndex: 0
    property var seasonEntries: []
    property bool routeRefreshScheduled: false

    focus: true
    readonly property bool directionRelease: true

    HoldNavigationController {
        id: rowHold
        initialRate: 3
        maximumRate: {
            const row = root.focusedMediaRow()
            return row ? Math.max(12, row.count / 5) : 12
        }
        cruiseDuration: 2000
        rampDuration: 500
        stepCallback: function (key, steps) {
            const row = root.focusedMediaRow()
            if (row)
                row.moveBy((key === Qt.Key_Left ? -1 : 1) * steps)
        }
    }

    component DetailAction: ActionButton {
        property string label: ""
        property bool primary: false
        property bool enabledButton: true
        signal activated

        width: primary ? Math.min(Math.max(implicitWidth, 190), 320) : Math.min(Math.max(implicitWidth, 132), 230)
        text: label
        kind: primary ? "primary" : "secondary"
        enabled: enabledButton
        onClicked: activated()
    }

    component IconAction: IconButton {
        property string label: ""
        property bool enabledButton: true
        signal activated

        width: Metrics.controlHeightPx
        height: width
        chromeless: true
        selected: checked
        enabled: enabledButton
        accessibleName: label
        onClicked: activated()
    }

    component MenuOption: ActionButton {
        property string label: ""
        property string externalUrl: ""
        signal activated

        width: parent ? parent.width : 280
        text: label
        kind: "flat"
        onClicked: activated()
    }

    component DetailLink: FocusScope {
        id: link
        property string label: ""
        property bool dropdown: false
        signal activated

        visible: label.length > 0
        implicitWidth: linkColumn.implicitWidth
        implicitHeight: linkColumn.implicitHeight
        focus: true

        Column {
            id: linkColumn
            anchors.left: parent.left
            anchors.top: parent.top
            spacing: 2

            Row {
                id: linkRow
                spacing: 8

                AppText {
                    id: linkText
                    anchors.verticalCenter: parent.verticalCenter
                    text: link.label
                    color: link.activeFocus || hover.hovered ? Theme.textPrimary : Theme.textSecondary
                    font.pixelSize: root.detailTitlePx
                    font.weight: Font.DemiBold
                    maximumLineCount: 1
                    elide: Text.ElideRight
                }

                MaterialIcon {
                    anchors.verticalCenter: linkText.verticalCenter
                    name: link.dropdown ? "expand_more" : "chevron_right"
                    iconSize: Math.max(20, Math.round(root.detailTitlePx * 0.45))
                    iconColor: link.activeFocus || hover.hovered ? Theme.accent : Theme.textMuted
                }
            }

            Rectangle {
                width: linkText.width
                height: link.activeFocus ? 3 : 2
                radius: height / 2
                color: link.activeFocus || hover.hovered ? Theme.accent : Theme.borderStrong
                opacity: link.activeFocus || hover.hovered ? 1 : 0.75
            }
        }

        HoverHandler {
            id: hover
        }
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            onClicked: link.activated()
        }
    }

    component MetadataPanel: FocusScope {
        id: panel
        property var rows: []
        property int currentRow: 0
        property int currentChip: 0
        property bool browsing: false
        readonly property bool hasRows: rows && rows.length > 0
        signal activated(string kind, var value)
        signal leaveUp
        signal leaveDown

        function focusPanel() {
            browsing = false
            InputKeys.focus(panel)
        }

        function chipText(value) {
            if (value === undefined || value === null)
                return ""
            if (typeof value === "string")
                return value
            return String(value.name || value.title || value.value || "")
        }

        function rowValues(row) {
            if (!hasRows || row < 0 || row >= rows.length)
                return []
            return rows[row].values || []
        }

        function normalizeSelection() {
            if (!hasRows) {
                currentRow = 0
                currentChip = 0
                browsing = false
                return
            }
            currentRow = Math.max(0, Math.min(currentRow, rows.length - 1))
            currentChip = Math.max(0, Math.min(currentChip, Math.max(0, rowValues(currentRow).length - 1)))
        }

        function moveRow(delta) {
            const next = currentRow + delta
            if (next < 0) {
                browsing = false
                leaveUp()
                return true
            }
            if (next >= rows.length) {
                browsing = false
                leaveDown()
                return true
            }
            currentRow = next
            currentChip = Math.min(currentChip, Math.max(0, rowValues(currentRow).length - 1))
            return true
        }

        function moveChip(delta) {
            const values = rowValues(currentRow)
            if (values.length <= 0)
                return true
            currentChip = Math.max(0, Math.min(currentChip + delta, values.length - 1))
            return true
        }

        function activateCurrent() {
            normalizeSelection()
            const values = rowValues(currentRow)
            if (values.length <= 0)
                return false
            activated(rows[currentRow].kind || "", values[currentChip])
            return true
        }

        function leaveBrowse() {
            browsing = false
            InputKeys.focus(panel)
        }

        function routeKey(key, phase, repeat) {
            if (!hasRows)
                return false
            normalizeSelection()
            if (!browsing) {
                if (key === Qt.Key_Up) {
                    leaveUp()
                    return true
                }
                if (key === Qt.Key_Down) {
                    leaveDown()
                    return true
                }
                return InputKeys.isHorizontal(key)
            }
            if (key === Qt.Key_Left)
                return moveChip(-1)
            if (key === Qt.Key_Right)
                return moveChip(1)
            if (key === Qt.Key_Up)
                return moveRow(-1)
            if (key === Qt.Key_Down)
                return moveRow(1)
            return false
        }

        function activate() {
            if (!browsing)
                browsing = true
            else
                activateCurrent()
        }

        function back() {
            if (!browsing)
                return false
            leaveBrowse()
            return true
        }

        width: parent ? parent.width : implicitWidth
        height: implicitHeight
        implicitHeight: visible ? block.implicitHeight : 0
        visible: hasRows
        focus: true
        onRowsChanged: normalizeSelection()

        Surface {
            id: block
            width: parent.width
            implicitHeight: metadataColumn.implicitHeight + 34
            height: implicitHeight
            focused: panel.activeFocus
            hovered: panelHover.hovered
            baseColor: Theme.floatingPanel
            radius: Theme.radiusPanel

            ColumnLayout {
                id: metadataColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 17
                spacing: 12

                Repeater {
                    model: panel.rows
                    delegate: RowLayout {
                        id: rowDelegate
                        required property int index
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 16

                        SecondaryText {
                            Layout.preferredWidth: Math.min(104, Math.max(72, block.width * 0.1))
                            text: String(rowDelegate.modelData.label || "")
                            color: panel.browsing && panel.currentRow === rowDelegate.index ? Theme.textPrimary :
                                                                                              Theme.textMuted

                            font.pixelSize: Metrics.metaSizePx
                            font.weight: Font.DemiBold
                            maximumLineCount: 1
                            elide: Text.ElideRight
                        }

                        Flow {
                            Layout.fillWidth: true
                            spacing: 8

                            Repeater {
                                model: rowDelegate.modelData.values || []
                                delegate: MetadataChip {
                                    id: chip
                                    required property int index
                                    required property var modelData
                                    text: panel.chipText(modelData)
                                    selected: panel.browsing && panel.currentRow === rowDelegate.index
                                              && panel.currentChip === index
                                    hovered: chipMouse.containsMouse

                                    MouseArea {
                                        id: chipMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        onClicked: panel.activated(rowDelegate.modelData.kind || "", modelData)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        HoverHandler {
            id: panelHover
        }
    }

    Component.onCompleted: {
        syncUserState()
        updateDetailCounts()
        rebuildSeasonEntries()
        scheduleActiveRouteRefresh()
    }

    onRouteActiveChanged: {
        if (routeActive)
        enterRoute(false)
        else
        App.cancelEpisodicPlaybackSelection()
    }

    onActiveFocusChanged: {
        if (activeFocus)
        focusDefaultAction()
        else
        rowHold.stopTracking()
    }
    function currentMediaItem() {
        return item
    }

    onRouteItemChanged: {
        seasonPickerOpen = false
        overflowOpen = false
        if (routeActive)
        enterRoute(true)
    }
    function enterRoute(resetFocus) {
        syncUserState()
        if (resetFocus)
            focusDefaultAction()
        scheduleActiveRouteRefresh()
    }

    function scheduleActiveRouteRefresh() {
        if (!routeActive || routeRefreshScheduled)
            return
        routeRefreshScheduled = true
        Qt.callLater(function () {
            root.routeRefreshScheduled = false
            if (!root.routeActive)
                return
            root.refreshDetailRows()
            root.refreshItemDetail()
        })
    }

    Connections {
        target: Content
        function onDetailRowsChanged() {
            root.updateDetailCounts()
            root.rebuildSeasonEntries()
        }
        function onDetailItemChanged() {
            if (root.fullDetailItem.movieId)
                root.syncUserState()
        }
    }

    Connections {
        target: ItemState
        function onFavoriteChanged(itemId, favorite) {
            if ((root.item.movieId || "") === itemId)
                root.favoriteState = favorite
        }
        function onPlayedChanged(itemId, played) {
            if ((root.item.movieId || "") === itemId)
                root.playedState = played
        }
    }

    Connections {
        target: Content.detailSeasons
        function onModelReset() {
            root.updateDetailCounts()
        }
        function onRowsInserted() {
            root.updateDetailCounts()
        }
        function onRowsRemoved() {
            root.updateDetailCounts()
        }
    }

    Connections {
        target: Content.detailSimilarItems
        function onModelReset() {
            root.updateDetailCounts()
        }
        function onRowsInserted() {
            root.updateDetailCounts()
        }
        function onRowsRemoved() {
            root.updateDetailCounts()
        }
    }

    Connections {
        target: Content.detailSeasonOptions
        function onModelReset() {
            root.rebuildSeasonEntries()
        }
        function onRowsInserted() {
            root.rebuildSeasonEntries()
        }
        function onRowsRemoved() {
            root.rebuildSeasonEntries()
        }
    }

    function back() {
        if (seasonPickerOpen) {
            closeSeasonPicker()
            return true
        }
        if (focusZone === "metadata" && metadataPanel.back())
            return true
        if (overflowOpen) {
            overflowOpen = false
            focusActionIndex(orderedActions().indexOf(menuAction))
            return true
        }
        return false
    }

    function updateDetailCounts() {
        if (contextRow)
            contextRow.currentIndex = contextCount > 0 ? Math.max(0, Math.min(contextRow.currentIndex, contextCount - 1)) :
                                                         0
        if (similarRow)
            similarRow.currentIndex = similarCount > 0 ? Math.max(0, Math.min(similarRow.currentIndex, similarCount - 1)) :
                                                         0
    }

    function refreshDetailRows() {
        const itemId = item.movieId || ""
        const key = itemId + ":" + typeText + ":" + seriesIdText + ":" + seasonIdText
        if (key === loadedDetailKey)
            return
        loadedDetailKey = key
        if (contextRow)
            contextRow.currentIndex = 0
        if (similarRow)
            similarRow.currentIndex = 0
        Content.loadDetailRows(itemId, typeText, seriesIdText, seasonIdText)
    }

    function refreshItemDetail() {
        const itemId = routeItem.movieId || ""
        if (itemId.length > 0)
            Content.loadItemDetail(itemId)
    }

    function rebuildSeasonEntries() {
        const entries = []
        let selected = 0
        for (let index = 0; index < seasonOptionCount; ++index) {
            const season = Content.detailSeasonOptions.get(index) || ({})
            const id = String(season.movieId || "")
            entries.push({
                             "label": String(season.title || "Season"),
                             "seasonId": id,
                             "modelIndex": index
                         })
            if (id === currentSeasonId)
                selected = index
        }
        seasonEntries = entries
        seasonPickerIndex = selected
    }

    function openSeasonPicker() {
        if (!showSeasonLink)
            return
        if (seasonEntries.length <= 1) {
            if (showContextRow && contextCount > 0)
                focusNamedZone("context")
            else
                focusFirstMediaRow()
            return
        }
        seasonPickerOpen = true
        Qt.callLater(function () {
            if (seasonPickerList)
                InputKeys.focus(seasonPickerList)
        })
    }

    function closeSeasonPicker() {
        seasonPickerOpen = false
        InputKeys.focus(seasonLink)
    }

    function selectSeason(entry) {
        if (!entry)
            return
        seasonPickerOpen = false
        const index = Number(entry.modelIndex)
        if (index < 0 || index >= seasonOptionCount)
            return
        if (String(entry.seasonId || "") === currentSeasonId) {
            InputKeys.focus(seasonLink)
            return
        }
        if (shell)
            shell.openDetailsAt(Content.detailSeasonOptions, index, "season-selector", detailsReturnRoute)
    }

    function syncUserState() {
        const current = item || ({})
        favoriteState = Boolean(current.favorite)
        playedState = Boolean(current.played)
    }

    function focusDefaultAction() {
        if (detailsFlick)
            detailsFlick.contentY = 0
        actionIndex = 0
        focusNamedZone("actions")
    }

    function focusZones() {
        const zones = []
        if (seriesLink.visible)
            zones.push("series")
        if (seasonLink.visible)
            zones.push("season")
        zones.push("actions")
        if (showMetadataPanel)
            zones.push("metadata")
        if (showContextRow && contextCount > 0)
            zones.push("context")
        if (showPeopleRow)
            zones.push("people")
        if (showSimilarRow)
            zones.push("similar")
        return zones
    }

    function zoneTarget(zone) {
        if (zone === "series")
            return seriesLink
        if (zone === "season")
            return seasonLink
        if (zone === "actions")
            return actionRow
        if (zone === "metadata")
            return metadataPanel
        if (zone === "context" || zone === "people" || zone === "similar")
            detailRowsLoader.forced = true
        if (zone === "context")
            return contextRow
        if (zone === "people")
            return peopleRow
        return similarRow
    }

    function focusedMediaRow() {
        if (focusZone === "context")
            return contextRow
        if (focusZone === "people")
            return peopleRow
        if (focusZone === "similar")
            return similarRow
        return null
    }

    function focusNamedZone(zone) {
        const target = zoneTarget(zone)
        if (!target)
            return false
        rowHold.stopTracking()
        focusZone = zone
        if (zone === "actions")
            focusActionIndex(actionIndex)
        else if (zone === "metadata")
            metadataPanel.focusPanel()
        else if (zone === "context")
            contextRow.focusList()
        else if (zone === "people")
            peopleRow.focusList()
        else if (zone === "similar")
            similarRow.focusList()
        else
            InputKeys.focus(target)
        InputKeys.positionChild(detailsFlick, target)
        return true
    }

    function moveFocusZone(delta) {
        const zones = focusZones()
        const current = Math.max(0, zones.indexOf(focusZone))
        const next = current + delta
        if (next < 0) {
            if (detailsFlick)
                detailsFlick.contentY = 0
            if (shell)
                shell.focusNavBar()
            return true
        }
        return next >= zones.length || focusNamedZone(zones[next])
    }

    function focusActionRow() {
        actionIndex = 0
        return focusNamedZone("actions")
    }

    function focusFirstMediaRow() {
        return moveFocusZone(1)
    }

    function orderedActions() {
        const actions = []
        if (showPrimaryAction)
            actions.push(primaryAction)
        if (showPrimaryAction && hasProgress)
            actions.push(restartAction)
        actions.push(playedAction)
        actions.push(favoriteAction)
        if (showContextPlaybackActions || mediaInfoAvailable || showLetterboxdAction || showExternalActions)
            actions.push(menuAction)
        return actions
    }

    function focusActionIndex(index) {
        const actions = orderedActions()
        if (actions.length === 0)
            return false
        focusZone = "actions"
        actionIndex = Math.max(0, Math.min(index, actions.length - 1))
        InputKeys.focus(actions[actionIndex])
        return true
    }

    function focusNextAction(delta) {
        return focusActionIndex(actionIndex + delta)
    }

    function activatePrimary(fromStart) {
        if (App.busy)
            return
        if (canPlayAlbum) {
            App.playModel(Content.detailSeasons, false)
            return
        }
        if (canPlayEpisodicContainer) {
            const seriesId = typeText === "Series" ? String(item.movieId || "") : seriesIdText
            App.playEpisodicContainer(seriesId, typeText === "Season" ? seasonIdText : "")
            return
        }
        if (selectedIndex < 0 || !canPlay)
            return
        App.playFromModel(itemModel, selectedIndex, fromStart === true)
    }

    function playDetailContext(shuffled) {
        if (!showContextPlaybackActions)
            return
        App.playModel(Content.detailSeasons, shuffled === true)
        overflowOpen = false
    }

    function toggleFavorite() {
        if (!item.movieId)
            return
        favoriteState = !favoriteState
        ItemState.setFavorite(item.movieId, favoriteState)
    }

    function togglePlayed() {
        if (!item.movieId)
            return
        playedState = !playedState
        ItemState.setPlayed(item.movieId, playedState)
    }

    function overflowOptions() {
        const options = []
        if (showContextPlaybackActions)
            options.push(playAllOption, shuffleOption)
        if (mediaInfoAvailable)
            options.push(mediaInfoOption)
        if (showLetterboxdAction)
            options.push(letterboxdOption)
        for (let index = 0; index < externalLinkOptions.count; ++index) {
            const option = externalLinkOptions.itemAt(index)
            if (option)
                options.push(option)
        }
        return options
    }

    function focusOverflow(index) {
        const options = overflowOptions()
        overflowIndex = Math.max(0, Math.min(index, options.length - 1))
        focusZone = "overflow"
        InputKeys.focus(options[overflowIndex])
    }

    function updateOverflowAnchor() {
        overflowAnchorPoint = menuAction.mapToItem(root, menuAction.width / 2, menuAction.height)
    }

    function toggleOverflow() {
        overflowOpen = !overflowOpen
        if (overflowOpen) {
            updateOverflowAnchor()
            Qt.callLater(function () {
                focusOverflow(0)
            })
        } else {
            focusActionIndex(orderedActions().indexOf(menuAction))
        }
    }

    function openMediaInfo() {
        overflowOpen = false
        if (shell)
            shell.openMediaInfo(item)
    }

    function openExternalUrl(url) {
        overflowOpen = false
        const target = String(url || "").trim()
        if (!Platform.isTV && target.toLowerCase().indexOf("https://") === 0)
            Qt.openUrlExternally(target)
    }

    function openLetterboxd() {
        if (showLetterboxdAction)
            openExternalUrl(letterboxdUrl)
    }

    function openSeriesLink() {
        if (seriesIdText.length <= 0)
            return
        if (shell)
            shell.openSeriesDetails(seriesIdText, seriesTitle, detailsReturnRoute)
    }

    function openSeasonLink() {
        openSeasonPicker()
    }

    function openContextItem(index) {
        if (index < 0)
            return
        if (typeText === "Series") {
            if (shell)
                shell.openDetailsAt(Content.detailSeasons, index, "season", detailsReturnRoute)
            return
        }
        if (albumDetail) {
            App.playFromModel(Content.detailSeasons, index)
            return
        }
        if (shell)
            shell.openDetailsAt(Content.detailSeasons, index, "context", detailsReturnRoute)
    }

    function openSimilarItem(index) {
        if (index >= 0 && shell)
            shell.openDetailsAt(Content.detailSimilarItems, index, "similar", detailsReturnRoute)
    }

    function openPerson(person) {
        if (person && person.id && shell)
            shell.openPerson(person)
    }

    function openNamedCollection(kind, value) {
        const name = chipText(value)
        if (!name)
            return
        App.openNamedCollection(kind, name, typeText === "MusicAlbum" || typeText === "MusicArtist" || typeText
                                === "Audio" ? "music" : "")
        if (shell)
            shell.replaceRoute("libraryGrid")
    }

    function routeKey(key, phase, repeat) {
        if (seasonPickerOpen)
            return seasonPickerList ? seasonPickerList.routeKey(key, phase, repeat) : true
        if (phase === "release") {
            if (rowHold.active)
                return rowHold.routeKey(key, phase, repeat)
            return true
        }
        if (focusZone === "overflow") {
            rowHold.stopTracking()
            const options = overflowOptions()
            if (key === Qt.Key_Up && overflowIndex > 0) {
                focusOverflow(overflowIndex - 1)
            } else if (key === Qt.Key_Down && overflowIndex + 1 < options.length) {
                focusOverflow(overflowIndex + 1)
            } else if (key === Qt.Key_Down) {
                overflowOpen = false
                focusZone = "actions"
                moveFocusZone(1)
            } else if (key === Qt.Key_Up || InputKeys.isHorizontal(key)) {
                overflowOpen = false
                focusActionIndex(orderedActions().indexOf(menuAction))
            } else {
                return false
            }
            return true
        }
        if (focusZone === "metadata") {
            rowHold.stopTracking()
            return metadataPanel.routeKey(key, phase, repeat)
        }
        if (InputKeys.isVertical(key)) {
            rowHold.stopTracking()
            if (focusZone === "actions" && key === Qt.Key_Down && overflowOpen && orderedActions()[actionIndex]
                    === menuAction) {
                focusOverflow(0)
                return true
            }
            return moveFocusZone(key === Qt.Key_Down ? 1 : -1)
        }
        if (focusZone === "actions") {
            rowHold.stopTracking()
            if (key === Qt.Key_Left)
                return focusNextAction(-1)
            if (key === Qt.Key_Right)
                return focusNextAction(1)
            return false
        }
        if (focusZone === "series" || focusZone === "season") {
            rowHold.stopTracking()
            return InputKeys.isHorizontal(key)
        }
        const row = focusedMediaRow()
        if (row && InputKeys.isHorizontal(key))
            return rowHold.routeKey(key, phase, repeat)
        rowHold.stopTracking()
        const target = zoneTarget(focusZone)
        return Boolean(target && target.routeKey && target.routeKey(key, phase, repeat))
    }

    function activate() {
        if (seasonPickerOpen) {
            if (seasonPickerList)
                seasonPickerList.activate()
        } else if (focusZone === "series") {
            openSeriesLink()
        } else if (focusZone === "season") {
            openSeasonLink()
        } else if (focusZone === "metadata") {
            metadataPanel.activate()
        } else if (focusZone === "context") {
            contextRow.activate()
        } else if (focusZone === "people") {
            peopleRow.activate()
        } else if (focusZone === "similar") {
            similarRow.activate()
        } else if (focusZone === "overflow") {
            const option = overflowOptions()[overflowIndex]
            if (option === playAllOption)
                playDetailContext(false)
            else if (option === shuffleOption)
                playDetailContext(true)
            else if (option === mediaInfoOption)
                openMediaInfo()
            else if (option === letterboxdOption)
                openLetterboxd()
            else if (option)
                openExternalUrl(option.externalUrl)
        } else {
            const action = orderedActions()[actionIndex]
            if (action === playedAction)
                togglePlayed()
            else if (action === favoriteAction)
                toggleFavorite()
            else if (action === menuAction)
                toggleOverflow()
            else if (action === restartAction)
                activatePrimary(true)
            else
                activatePrimary(false)
        }
    }

    function longPress() {
        if (focusZone === "context")
            return contextRow.longPress()
        if (focusZone === "similar")
            return similarRow.longPress()
        return focusZone === "actions" && shell ? shell.openItemMenu(item, orderedActions()[actionIndex], {
                                                                         "deferBackdropDismissal": true
                                                                     }) : false
    }

    function primaryLabel() {
        return canPlayAlbum ? "Play album" : (item.playActionLabel || "Play")
    }

    function runtimeText(ticks) {
        ticks = Number(ticks || 0)
        if (ticks <= 0)
            return ""
        const minutes = Math.round(ticks / 600000000)
        const hours = Math.floor(minutes / 60)
        const mins = minutes % 60
        return hours > 0 ? hours + "h " + mins + "m" : mins + "m"
    }

    function remainingText() {
        const remaining = Number(item.runtimeTicks || 0) - Number(item.resumeTicks || 0)
        return remaining > 0 ? runtimeText(remaining) + " left" : ""
    }

    function progressText() {
        const watched = runtimeText(item.resumeTicks)
        const total = runtimeText(item.runtimeTicks)
        if (watched.length <= 0)
            return ""
        return total.length > 0 ? watched + " of " + total : watched
    }

    function twoDigit(value) {
        const n = Number(value || 0)
        return n > 0 && n < 10 ? "0" + n : (n > 0 ? String(n) : "")
    }

    function seasonTitle() {
        if (typeText === "Season" && item.title)
            return item.title
        const season = Number(item.seasonNumber || 0)
        return season > 0 ? "Season " + season : "Season"
    }

    function contextRowTitle() {
        if (typeText === "Series")
            return "Seasons"
        if (typeText === "BoxSet")
            return "Collection"
        if (typeText === "MusicAlbum")
            return "Tracks"
        if (typeText === "Season")
            return "Episodes"
        return seasonTitleText !== "Season" ? "More from " + seasonTitleText : "More from this season"
    }

    function yearFromDate(value) {
        if (!value || value.length < 4)
            return 0
        const parsed = parseInt(String(value).slice(0, 4), 10)
        return isNaN(parsed) ? 0 : parsed
    }

    function yearRange() {
        const start = Number(item.year || 0) > 0 ? Number(item.year) : yearFromDate(item.premiereDate)
        const end = yearFromDate(item.endDate)
        if (typeText === "Series" && start > 0 && end > 0 && end !== start)
            return start + " - " + end
        return start > 0 ? String(start) : ""
    }

    function ratingText() {
        const parts = []
        if (item.officialRating)
            parts.push(item.officialRating)
        const community = Number(item.communityRating || 0)
        if (community > 0)
            parts.push(community.toFixed(1))
        return parts.join(" / ")
    }

    function metadataLine() {
        const parts = []
        const years = yearRange()
        const rating = ratingText()
        const runtime = runtimeText(item.runtimeTicks)
        if (years.length > 0)
            parts.push(years)
        if (runtime.length > 0)
            parts.push(runtime)
        if (rating.length > 0)
            parts.push(rating)
        if (typeText.length > 0)
            parts.push(typeText)
        return parts.join(" / ")
    }

    function peopleByType(type) {
        const result = []
        for (let i = 0; i < metadataPeople.length; ++i) {
            const person = metadataPeople[i] || ({})
            if (String(person.type || "") === type)
                result.push(person)
        }
        return result
    }

    function valuesFromStrings(values) {
        const result = []
        if (!values)
            return result
        for (let i = 0; i < values.length; ++i) {
            const text = String(values[i] || "")
            if (text.length > 0)
                result.push(text)
        }
        return result
    }

    function buildMetadataRows() {
        const rows = []
        const directors = peopleByType("Director")
        const writers = peopleByType("Writer")
        const genres = valuesFromStrings(genreList)
        const studios = valuesFromStrings(studioList)
        if (directors.length > 0)
            rows.push({
                          label: directors.length === 1 ? "Director" : "Directors",
                          kind: "person",
                          values: directors
                      })
        if (writers.length > 0)
            rows.push({
                          label: writers.length === 1 ? "Writer" : "Writers",
                          kind: "person",
                          values: writers
                      })
        if (genres.length > 0)
            rows.push({
                          label: genres.length === 1 ? "Genre" : "Genres",
                          kind: "genre",
                          values: genres
                      })
        if (studios.length > 0)
            rows.push({
                          label: studios.length === 1 ? "Studio" : "Studios",
                          kind: "studio",
                          values: studios
                      })
        return rows
    }

    function chipText(value) {
        if (value === undefined || value === null)
            return ""
        if (typeof value === "string")
            return value
        return String(value.name || value.title || value.value || "")
    }

    function activateMetadata(kind, value) {
        if (kind === "genre" || kind === "studio")
            openNamedCollection(kind, value)
        else if (kind === "person")
            openPerson(value)
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    Image {
        anchors.fill: parent
        source: root.backgroundArt
        fillMode: Image.PreserveAspectCrop
        opacity: status === Image.Ready ? 0.34 : 0
        asynchronous: true
        cache: true
    }

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop {
                position: 0.0
                color: Theme.backdropScrimTop
            }
            GradientStop {
                position: 0.48
                color: Theme.backdropScrimMiddle
            }
            GradientStop {
                position: 1.0
                color: Theme.bg
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop {
                position: 0.0
                color: Theme.backdropScrimLeft
            }
            GradientStop {
                position: 0.55
                color: Theme.bg
            }
            GradientStop {
                position: 1.0
                color: Theme.backdropScrimRight
            }
        }
    }

    Flickable {
        id: detailsFlick
        anchors.fill: parent
        anchors.bottomMargin: technicalInfoBar.visible ? technicalInfoBar.height : 0
        contentWidth: width
        contentHeight: Math.max(height, contentColumn.implicitHeight + root.contentMargin)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        FastWheelHandler {
            flickable: detailsFlick
        }

        Behavior on contentY {
            enabled: !Theme.reducedMotion
            NumberAnimation {
                duration: 90
                easing.type: Easing.OutCubic
            }
        }

        ColumnLayout {
            id: contentColumn
            width: detailsFlick.width
            height: implicitHeight
            spacing: Metrics.sectionGapPx

            Item {
                id: hero
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(heroCopy.y + heroCopy.implicitHeight + root.contentMargin * 0.5,
                                                 stillPanel.visible ? stillPanel.y + stillPanel.height
                                                                      + root.contentMargin * 0.5 : 0)

                ColumnLayout {
                    id: heroCopy
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.leftMargin: root.contentMargin
                    anchors.topMargin: root.compactEpisodicDetail ? Math.max(root.smallZoom ? 16 : 12, root.height * (
                                                                                 root.smallZoom ? 0.02 : 0.016)) :
                                                                    Math.max(root.smallZoom ? 28 : 22, root.height * (
                                                                                 root.smallZoom ? 0.038 : 0.032))
                    width: root.copyWidth
                    spacing: root.compactEpisodicDetail ? 8 : 14

                    DetailLink {
                        id: seriesLink
                        label: root.showSeriesLink ? root.seriesTitle : ""
                        onActivated: root.openSeriesLink()
                    }

                    DetailLink {
                        id: seasonLink
                        label: root.showSeasonLink ? root.seasonTitleText : ""
                        dropdown: root.seasonEntries.length > 1
                        onActivated: root.openSeasonLink()
                    }

                    AppText {
                        Layout.fillWidth: true
                        visible: root.typeText !== "Season"
                        text: root.typeText === "Episode" && root.twoDigit(root.item.episodeNumber).length > 0 ? "E"
                                                                                                                 + root.twoDigit(
                                                                                                                     root.item.episodeNumber)
                                                                                                                 + " " + root.titleText :
                                                                                                                 root.titleText
                        font.pixelSize: root.detailTitlePx
                        font.weight: Font.DemiBold
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                        lineHeight: 0.96
                    }

                    TechMetadataLine {
                        Layout.fillWidth: true
                        visible: root.metadataLine().length > 0
                        metadata: root.metadataLine()
                    }

                    AppText {
                        Layout.fillWidth: true
                        Layout.topMargin: root.compactEpisodicDetail ? 2 : 8
                        visible: Boolean(root.item.overview && root.item.overview.length > 0)
                        text: root.item.overview || ""
                        color: Theme.textSecondary
                        wrapMode: Text.Wrap
                        font.pixelSize: Metrics.bodySizePx + 1
                        lineHeight: 1.18
                        maximumLineCount: root.compactEpisodicDetail ? (root.typeText === "Season" ? 3 : 4) : 5
                        elide: Text.ElideRight
                    }

                    Row {
                        id: actionRow
                        Layout.topMargin: root.compactEpisodicDetail ? 8 : 18
                        spacing: 10

                        DetailAction {
                            id: primaryAction
                            iconName: "play_arrow"
                            label: root.primaryLabel()
                            primary: true
                            visible: root.showPrimaryAction
                            enabledButton: root.showPrimaryAction && !App.busy
                            onActivated: root.activatePrimary(false)
                        }

                        DetailAction {
                            id: restartAction
                            iconName: "replay"
                            label: "Start from beginning"
                            visible: !root.canPlayEpisodicContainer && root.showPrimaryAction && root.hasProgress
                            enabledButton: visible && !App.busy
                            onActivated: root.activatePrimary(true)
                        }

                        IconAction {
                            id: playedAction
                            iconName: root.playedState ? "visibility" : "visibility_off"
                            label: root.playedState ? "Mark unwatched" : "Mark watched"
                            checked: root.playedState
                            enabledButton: root.selectedIndex >= 0
                            onActivated: root.togglePlayed()
                        }

                        IconAction {
                            id: favoriteAction
                            iconName: root.favoriteState ? "favorite" : "favorite_border"
                            label: root.favoriteState ? "Remove favourite" : "Add favourite"
                            checked: root.favoriteState
                            enabledButton: root.selectedIndex >= 0
                            onActivated: root.toggleFavorite()
                        }

                        IconAction {
                            id: menuAction
                            iconName: "menu"
                            label: "More"
                            checked: root.overflowOpen
                            visible: root.showContextPlaybackActions || root.mediaInfoAvailable
                                     || root.showLetterboxdAction || root.showExternalActions
                            enabledButton: root.selectedIndex >= 0
                            onActivated: root.toggleOverflow()
                        }
                    }

                    SecondaryText {
                        Layout.fillWidth: true
                        visible: root.hasProgress && root.remainingText().length > 0
                        text: root.progressText() + " / " + root.remainingText()
                        color: Theme.textMuted
                        font.pixelSize: Metrics.metaSizePx
                    }

                    MetadataPanel {
                        id: metadataPanel
                        Layout.fillWidth: true
                        Layout.topMargin: root.compactEpisodicDetail ? 12 : 28
                        rows: root.compactEpisodicDetail ? root.metadataRows.slice(0, 2) : root.metadataRows
                        onActivated: (kind, value) => root.activateMetadata(kind, value)
                        onLeaveUp: root.focusActionRow()
                        onLeaveDown: root.focusFirstMediaRow()
                    }
                }

                Item {
                    id: stillPanel
                    visible: root.showSideArt
                    anchors.left: heroCopy.right
                    anchors.leftMargin: Metrics.sectionGapPx * 2
                    anchors.top: heroCopy.top
                    width: Math.max(0, Math.min(root.sideArtWidth, parent.width - x - root.contentMargin))
                    height: (root.albumDetail ? width : Math.round(width * 9 / 16)) + (root.hasProgress ? 46 : 0)

                    ImageCard {
                        id: stillCard
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        height: root.albumDetail ? width : Math.round(width * 9 / 16)
                        imageUrl: root.stillArt
                        fallbackText: root.typeText
                    }

                    Rectangle {
                        anchors.left: stillCard.left
                        anchors.right: stillCard.right
                        anchors.bottom: stillCard.bottom
                        height: 5
                        visible: root.hasProgress
                        color: Theme.border
                        Rectangle {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: parent.width * Math.max(0, Math.min(1, root.item.progress || 0))
                            color: Theme.accent
                        }
                    }

                    RowLayout {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: stillCard.bottom
                        anchors.topMargin: 12
                        visible: root.hasProgress

                        SecondaryText {
                            Layout.fillWidth: true
                            text: root.progressText()
                            color: Theme.textMuted
                            font.pixelSize: Metrics.metaSizePx
                            elide: Text.ElideRight
                        }
                        SecondaryText {
                            text: root.remainingText()
                            color: Theme.textMuted
                            font.pixelSize: Metrics.metaSizePx
                        }
                    }
                }
            }

            Loader {
                id: detailRowsLoader
                Layout.fillWidth: true
                property bool forced: false
                readonly property int estimatedRowHeight: Math.round(root.rowPosterWidth * 1.5) + Metrics.scaled(94)
                readonly property int estimatedRows: Number(root.showContextRow || root.reserveContextRow) + Number(
                                                         root.showPeopleRow) + Number(root.showSimilarRow)
                active: forced || detailsFlick.contentY + detailsFlick.height * 1.5 >= y
                // zoneTarget() needs the rows to exist synchronously when it
                // forces them for focus; flipping asynchronous off completes
                // a pending incubation in place.
                asynchronous: !forced
                Layout.preferredHeight: item ? item.implicitHeight : estimatedRows * estimatedRowHeight

                sourceComponent: Item {
                    id: detailRowsArea
                    property alias contextRow: contextRowItem
                    property alias peopleRow: peopleRowItem
                    property alias similarRow: similarRowItem
                    readonly property int rowSpacing: Metrics.sectionGapPx
                    readonly property int visibleRowCount: (contextRowItem.visible ? 1 : 0) + (peopleRowItem.visible
                                                                                               ? 1 : 0) + (
                                                               similarRowItem.visible ? 1 : 0)
                    readonly property int rowsHeight: contextRowItem.height + peopleRowItem.height
                                                      + similarRowItem.height + Math.max(0, visibleRowCount - 1)
                                                      * rowSpacing
                    readonly property int contextY: 0
                    readonly property int peopleY: contextY + (contextRowItem.visible ? contextRowItem.height + rowSpacing :
                                                                                        0)
                    readonly property int similarY: peopleY + (peopleRowItem.visible ? peopleRowItem.height + rowSpacing :
                                                                                       0)
                    implicitHeight: rowsHeight + root.contentMargin

                    MediaRow {
                        id: contextRowItem
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: root.contentMargin
                        anchors.rightMargin: root.contentMargin
                        y: detailRowsArea.contextY
                        title: root.contextRowTitle()
                        model: Content.detailSeasons
                        shell: root.shell
                        cardWidth: root.contextPosterCards ? root.rowPosterWidth : root.rowLandscapeWidth
                        cardKind: root.contextPosterCards ? "poster" : "landscape"
                        cardGap: root.rowGap
                        enabledRow: root.showContextRow
                        reserveWhenEmpty: root.reserveContextRow
                        loading: root.reserveContextRow
                        emptyText: root.typeText === "Series" ? "Loading seasons..." : (root.typeText === "BoxSet"
                                                                                        ? "Loading collection..." :
                                                                                          root.typeText
                                                                                          === "MusicAlbum"
                                                                                          ? "Loading tracks..." :
                                                                                            "Loading episodes...")
                        useSeriesPoster: root.typeText === "Series"
                        preferEpisodeTitle: !root.contextPosterCards
                        onActivated: index => root.openContextItem(index)
                    }

                    MediaRow {
                        id: peopleRowItem
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: root.contentMargin
                        anchors.rightMargin: root.contentMargin
                        y: detailRowsArea.peopleY
                        title: "Cast & Crew"
                        model: root.people
                        shell: root.shell
                        cardKind: "person"
                        cardWidth: root.rowPosterWidth
                        cardGap: root.rowGap
                        enabledRow: root.showPeopleRow
                        reserveWhenEmpty: root.reservePeopleRow
                        onActivated: (index, person) => root.openPerson(person)
                    }

                    MediaRow {
                        id: similarRowItem
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: root.contentMargin
                        anchors.rightMargin: root.contentMargin
                        y: detailRowsArea.similarY
                        title: "More Like This"
                        model: Content.detailSimilarItems
                        shell: root.shell
                        cardWidth: root.typeText === "Episode" ? root.rowLandscapeWidth : root.rowPosterWidth
                        cardKind: root.typeText === "Episode" ? "landscape" : "poster"
                        cardGap: root.rowGap
                        enabledRow: root.showSimilarRow
                        reserveWhenEmpty: root.reserveSimilarRow
                        useSeriesPoster: root.typeText !== "Episode"
                        preferEpisodeTitle: root.typeText === "Episode"
                        onActivated: index => root.openSimilarItem(index)
                    }
                }
            }
        }
    }

    TechnicalDetailsBar {
        id: technicalInfoBar
        visible: root.technicalInfo !== null && Theme.technicalMetadataMode === "Always" && hasContent
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: Math.max(0, parent.width - root.contentMargin)
        height: implicitHeight
        info: root.technicalInfo
        z: 18
    }

    MouseArea {
        anchors.fill: parent
        visible: root.overflowOpen
        z: 39
        onClicked: {
            root.overflowOpen = false
            root.focusActionIndex(root.orderedActions().indexOf(menuAction))
        }
    }

    PopupMenuPanel {
        id: overflowMenu
        width: Math.min(Metrics.scaled(292), root.width - root.contentMargin * 2)
        x: Math.max(root.contentMargin, Math.min(root.width - width - root.contentMargin, root.overflowAnchorPoint.x
                                                 - width / 2))
        y: root.overflowAnchorPoint.y + Metrics.scaled(8)
        open: root.overflowOpen
        openHeight: menuColumn.implicitHeight + Metrics.scaled(14)
        baseColor: Theme.floatingPanel
        z: 40

        Column {
            id: menuColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Metrics.scaled(7)

            MenuOption {
                id: playAllOption
                visible: root.showContextPlaybackActions
                iconName: "play_arrow"
                label: "Play all"
                onActivated: root.playDetailContext(false)
            }

            MenuOption {
                id: shuffleOption
                visible: root.showContextPlaybackActions
                iconName: "shuffle"
                label: "Shuffle play"
                onActivated: root.playDetailContext(true)
            }

            MenuOption {
                id: mediaInfoOption
                visible: root.mediaInfoAvailable
                iconName: "info"
                label: "Media info"
                onActivated: root.openMediaInfo()
            }

            MenuOption {
                id: letterboxdOption
                visible: root.showLetterboxdAction
                iconName: "open_in_new"
                label: "Open in Letterboxd"
                onActivated: root.openLetterboxd()
            }

            Repeater {
                id: externalLinkOptions
                model: root.externalLinks

                delegate: MenuOption {
                    required property var modelData
                    externalUrl: String(modelData.url || "")
                    iconName: "open_in_new"
                    label: "Open in " + String(modelData.name || "")
                    onActivated: root.openExternalUrl(externalUrl)
                }
            }
        }
    }

    LazyMenuPanel {
        id: seasonPickerPanel
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: root.contentMargin
        anchors.topMargin: Math.min(root.height - maximumHeight - root.contentMargin, root.contentMargin
                                    + root.detailTitlePx * 2.25)
        width: Math.min(Metrics.menuPanelWidth(root.width, 380), root.width - root.contentMargin * 2)
        maximumHeight: Math.min(Metrics.scaled(430), root.height - root.contentMargin * 2)
        z: 40
        open: root.seasonPickerOpen
        model: root.seasonEntries
        currentIndex: root.seasonPickerIndex
        edgeEscapeItem: seasonLink
        title: root.seriesTitle
        checkedFor: function (entry) {
            return entry && String(entry.seasonId || "") === root.currentSeasonId
        }
        onCurrentIndexChanged: root.seasonPickerIndex = currentIndex
        onDismissed: root.closeSeasonPicker()
        onAccepted: entry => root.selectSeason(entry)
    }
}
