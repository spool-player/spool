pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtQuick.Shapes
import "../theme"
import "../primitives"
import "LibraryNavigation.js" as LibraryNavigation
import "../shell/ItemActivation.js" as ItemActivation

FocusScope {
    id: root
    property var shell
    property var uiTransitionToken: 0
    property bool listMode: false
    property string touchPreviewItemId: ""
    readonly property string lane: Metrics.lane(width)
    property int columns: listMode ? 1 : Metrics.columns(width)
    readonly property bool largeZoom: Metrics.uiScalePercent >= 150
    readonly property bool smallZoom: Metrics.uiScalePercent <= 100
    readonly property int contentTopMargin: Math.max(Metrics.scaled(8), Math.round(Metrics.pageMarginPx * 0.4))
    property bool sortOpen: false
    property bool filtersOpen: false
    property bool libraryOpen: false
    property int sortIndex: 0
    property int filterIndex: 0
    property string expandedFilterSection: ""
    property int libraryIndex: 0
    property var sortEntries: buildSortEntries()
    property var filterEntries: []
    property var libraryEntries: []
    property string typeAheadBuffer: ""
    readonly property bool directionRelease: true
    readonly property int alphabetFeedbackIndex: {
        if (grid.heldKey && grid.currentIndex >= 0)
        return grid.currentIndex
        return grid.topLeftVisibleIndex()
    }
    readonly property string currentAlphabetLabel: {
        if (!Browse.items || alphabetFeedbackIndex < 0 || alphabetFeedbackIndex >= Browse.items.count)
        return "#"
        return LibraryNavigation.sectionLabel(currentSortBy(), Browse.items.get(alphabetFeedbackIndex))
    }
    readonly property var libraryList: libraryPanel.menuList
    readonly property var sortList: sortPanel.menuList
    readonly property var filterList: filterPanel.menuList
    readonly property string collectionType: Browse.libraryCollectionType
    // Music browses cover art, which is square: poster cells would crop it.
    // Artist and album browses arrive without a collection type, so read the
    // content itself; an empty page falls back to the library it came from.
    readonly property bool squareArtwork: {
        if (Browse.items && Browse.items.count > 0)
        return ["MusicAlbum", "MusicArtist", "Audio"].indexOf(String(Browse.items.get(0).itemType || "")) >= 0
        return collectionType === "music"
    }
    readonly property real artworkAspect: squareArtwork ? 1 : 1.5
    readonly property var libraryQuery: Browse.query
    readonly property var filterOptions: Browse.filterOptions
    readonly property int activeFilterCount: Browse.filterActiveCount
    readonly property bool browseLoading: Browse.loadingMore
    // Fixed descriptor-backed pages reuse this grid but have no library
    // switcher, sort, or filter controls.
    readonly property bool isFixedBrowseView: ["genre", "studio", "playlist", "boxset", "folder", "artist"].indexOf(
        Browse.viewKind) >= 0
    onIsFixedBrowseViewChanged: if (isFixedBrowseView)
    closeMenus()
    focus: true
    // An empty library is an answer, not a wait. The page is showing its empty
    // state and the transition that brought it up is over; requiring a
    // delegate that was never coming used to hold that transition open until
    // the route host gave up on it.
    readonly property bool contentReady: grid.count === 0 || gridReveal.firstDelegateReady
    // Grid position to restore across model resets (sort/filter/library
    // changes); the page itself is resident, so this survives navigation.
    property int savedIndex: shell ? Number(shell.routeArgs.focusIndex || 0) : 0
    property bool navigationFocusVisible: true
    property bool pointerNavigationPending: false
    property var pendingScrollController: null
    Component.onCompleted: if (activeFocus)
    InputKeys.focus(grid)
    onActiveFocusChanged: {
        clearPendingPointerNavigation()
        if (activeFocus)
        InputKeys.focus(grid)
    }

    component ToolbarButton: FocusScope {
        id: buttonRoot
        property string iconName: "filter_list"
        property string label: ""
        property bool checked: false
        property bool badge: false
        signal activated

        implicitWidth: Math.max(Metrics.scaled(118), labelText.implicitWidth + Metrics.scaled(58))
        implicitHeight: Metrics.scaled(42)
        focus: true

        HoverHandler {
            id: hover
        }

        Rectangle {
            anchors.fill: parent
            radius: Theme.radiusSmall
            color: buttonRoot.checked ? Theme.accentPanel : (buttonRoot.activeFocus || hover.hovered) ? Theme.bgHover :
                                                                                                        Theme.bgRaised
            border.width: buttonRoot.activeFocus ? 2 : 1
            border.color: buttonRoot.activeFocus ? Theme.textPrimary : buttonRoot.checked ? Theme.accent : Theme.border
            antialiasing: true
        }

        Row {
            anchors.centerIn: parent
            height: parent.height
            spacing: 8
            MaterialIcon {
                anchors.verticalCenter: parent.verticalCenter
                name: buttonRoot.iconName
                iconSize: 21
                iconColor: buttonRoot.checked ? Theme.accent : Theme.textPrimary
            }
            AppText {
                id: labelText
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                text: buttonRoot.label
                font.pixelSize: Metrics.metaSizePx + 1
                font.weight: Font.Medium
                maximumLineCount: 1
                elide: Text.ElideRight
            }
        }

        Rectangle {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 5
            width: 8
            height: 8
            radius: 4
            color: Theme.accent
            visible: buttonRoot.badge
        }

        MouseArea {
            anchors.fill: parent
            onClicked: buttonRoot.activated()
        }
    }

    function libraryCount() {
        return Libraries.rowCount()
    }

    function currentLibraryModelIndex() {
        const currentId = String(Browse.libraryId || "")
        const count = libraryCount()
        for (let i = 0; i < count; ++i) {
            const library = Libraries.get(i)
            if (library && String(library.libraryId || "") === currentId)
                return i
        }
        return count > 0 ? 0 : -1
    }

    function buildLibraryEntries() {
        const entries = []
        for (let index = 0; index < libraryCount(); ++index) {
            const library = Libraries.get(index)
            entries.push({
                             "label": String(library && library.name || ""),
                             "libraryId": String(library && library.libraryId || ""),
                             "index": index
                         })
        }
        return entries
    }

    function headerDetail() {
        const total = Browse.totalCount
        const count = Browse.items ? Browse.items.count : 0
        const parts = []
        if (count > 0)
            parts.push(count + (total > count ? " of " + total : "") + " items")
        if (activeFilterCount > 0)
            parts.push(activeFilterCount + " filter" + (activeFilterCount === 1 ? "" : "s"))
        return parts.join(" · ")
    }

    function listForKey(key) {
        const value = libraryQuery ? libraryQuery[key] : undefined
        if (value === undefined || value === null)
            return []
        if (Array.isArray(value))
            return value
        if (typeof value !== "string" && value.length !== undefined) {
            const result = []
            for (let i = 0; i < value.length; ++i)
                result.push(String(value[i]))
            return result
        }
        return String(value).split(",").filter(function (v) {
            return v.length > 0
        })
    }

    function listHas(key, value) {
        return listForKey(key).indexOf(String(value)) >= 0
    }

    function boolHas(key, value) {
        const current = libraryQuery ? libraryQuery[key] : undefined
        if (current === undefined || current === null)
            return false
        return Boolean(current) === Boolean(value)
    }

    function dynamicList(key) {
        const value = filterOptions ? filterOptions[key] : undefined
        if (!value)
            return []
        if (Array.isArray(value))
            return value
        if (value.length !== undefined) {
            const result = []
            for (let i = 0; i < value.length; ++i)
                result.push(value[i])
            return result
        }
        return []
    }

    function isSeriesLibrary() {
        return collectionType === "tvshows"
    }

    function isMovieLikeLibrary() {
        return collectionType === "movies" || collectionType === "homevideos" || collectionType === "musicvideos"
                || collectionType.length === 0
    }

    function currentSortBy() {
        return libraryQuery && libraryQuery.sortBy ? String(libraryQuery.sortBy) : "SortName"
    }

    function currentSortOrder() {
        return libraryQuery && libraryQuery.sortOrder ? String(libraryQuery.sortOrder) : "Ascending"
    }

    function currentSortLabel() {
        const by = currentSortBy()
        for (let i = 0; i < sortEntries.length; ++i)
            if (sortEntries[i].value === by)
                return sortEntries[i].label
        return "Sort"
    }

    function buildSortEntries() {
        const entries = [
                  {
                      label: "Name",
                      value: "SortName"
                  },
                  {
                      label: "Random",
                      value: "Random"
                  },
                  {
                      label: "Community rating",
                      value: "CommunityRating"
                  },
                  {
                      label: "Date added",
                      value: "DateCreated"
                  },
                  {
                      label: "Date played",
                      value: isSeriesLibrary() ? "SeriesDatePlayed" : "DatePlayed"
                  },
                  {
                      label: "Parental rating",
                      value: "OfficialRating"
                  },
                  {
                      label: "Release date",
                      value: "PremiereDate"
                  }
              ]
        if (isSeriesLibrary())
            entries.splice(4, 0, {
                               label: "Date episode added",
                               value: "DateLastContentAdded"
                           })
        else
            entries.splice(3, 0, {
                               label: "Critic rating",
                               value: "CriticRating"
                           })
        entries.push({
                         label: "Play count",
                         value: "PlayCount"
                     })
        entries.push({
                         label: "Runtime",
                         value: "Runtime"
                     })
        return [
                    {
                        label: currentSortOrder(),
                        iconName: currentSortOrder() === "Ascending" ? "arrow_upward" : "arrow_downward",
                        value: currentSortOrder() === "Ascending" ? "order:Descending" : "order:Ascending"
                    },
                    {
                        section: true,
                        label: "Sort by"
                    }
                ].concat(entries)
    }

    function addSection(entries, title) {
        entries.push({
                         section: true,
                         label: title
                     })
    }

    function addListFilter(entries, section, label, key, value) {
        entries.push({
                         section: false,
                         sectionName: section,
                         label: label,
                         key: key,
                         value: String(value),
                         kind: "list",
                         checked: listHas(key, value)
                     })
    }

    function addBoolFilter(entries, section, label, key) {
        entries.push({
                         section: false,
                         sectionName: section,
                         label: label,
                         key: key,
                         kind: "bool",
                         checked: boolHas(key, true)
                     })
    }

    function addNullableBoolFilter(entries, section, label, key, value) {
        entries.push({
                         section: false,
                         sectionName: section,
                         label: label,
                         key: key,
                         value: value,
                         kind: "nullableBool",
                         checked: boolHas(key, value)
                     })
    }

    function buildFilterEntries() {
        const entries = []
        addSection(entries, "Status")
        addListFilter(entries, "Status", "Played", "filters", "IsPlayed")
        addListFilter(entries, "Status", "Unplayed", "filters", "IsUnplayed")
        addListFilter(entries, "Status", "Favorite", "filters", "IsFavorite")
        addListFilter(entries, "Status", "Continue watching", "filters", "IsResumable")

        if (collectionType === "movies")
            addListFilter(entries, "Status", "Collections", "includeItemTypes", "BoxSet")
        if (isMovieLikeLibrary()) {
            addSection(entries, "Video")
            addNullableBoolFilter(entries, "Video", "HD", "isHd", true)
            addNullableBoolFilter(entries, "Video", "SD", "isHd", false)
            addBoolFilter(entries, "Video", "4K", "is4K")
            addBoolFilter(entries, "Video", "3D", "is3D")
            addListFilter(entries, "Video", "DVD", "videoTypes", "Dvd")
            addListFilter(entries, "Video", "Blu-ray", "videoTypes", "BluRay")
            addListFilter(entries, "Video", "ISO", "videoTypes", "Iso")
        }

        addSection(entries, "Features")
        addBoolFilter(entries, "Features", "Subtitles", "hasSubtitles")
        addBoolFilter(entries, "Features", "Trailers", "hasTrailer")
        addBoolFilter(entries, "Features", "Extras", "hasSpecialFeature")
        addBoolFilter(entries, "Features", "Theme songs", "hasThemeSong")
        addBoolFilter(entries, "Features", "Theme videos", "hasThemeVideo")

        if (isSeriesLibrary()) {
            addSection(entries, "Series status")
            addListFilter(entries, "Series status", "Continuing", "seriesStatus", "Continuing")
            addListFilter(entries, "Series status", "Ended", "seriesStatus", "Ended")
            addListFilter(entries, "Series status", "Unreleased", "seriesStatus", "Unreleased")
        }

        const genres = dynamicList("genres")
        if (genres.length > 0) {
            addSection(entries, "Genres")
            for (let i = 0; i < genres.length; ++i)
                addListFilter(entries, "Genres", String(genres[i]), "genres", String(genres[i]))
        }

        const ratings = dynamicList("officialRatings")
        if (ratings.length > 0) {
            addSection(entries, "Parental ratings")
            for (let i = 0; i < ratings.length; ++i)
                addListFilter(entries, "Parental ratings", String(ratings[i]), "officialRatings", String(ratings[i]))
        }

        const tags = dynamicList("tags")
        if (tags.length > 0) {
            addSection(entries, "Tags")
            for (let i = 0; i < tags.length; ++i)
                addListFilter(entries, "Tags", String(tags[i]), "tags", String(tags[i]))
        }

        const years = dynamicList("years")
        if (years.length > 0) {
            addSection(entries, "Years")
            for (let i = 0; i < years.length; ++i)
                addListFilter(entries, "Years", String(years[i]), "years", String(years[i]))
        }

        const studios = dynamicList("studios")
        if (studios.length > 0) {
            addSection(entries, "Studios")
            for (let i = 0; i < studios.length; ++i)
                addListFilter(entries, "Studios", studios[i].name || "", "studioIds", studios[i].id || "")
        }
        const grouped = []
        for (let index = 0; index < entries.length; ) {
            const sectionName = entries[index].label
            const options = []
            let selected = 0;
            ++index
            while (index < entries.length && !entries[index].section) {
                options.push(entries[index])
                if (entries[index].checked)
                    ++selected
                ++index
            }
            grouped.push({
                             label: sectionName,
                             detail: selected > 0 ? selected + " selected" : "",
                             sectionName: sectionName,
                             kind: "category",
                             iconName: expandedFilterSection === sectionName ? "expand_less" : "expand_more"
                         })
            if (expandedFilterSection === sectionName)
                for (let optionIndex = 0; optionIndex < options.length; ++optionIndex)
                    grouped.push(options[optionIndex])
        }
        return grouped
    }

    function firstActionableFilterIndex(entries) {
        for (let i = 0; i < entries.length; ++i) {
            if (!entries[i].section)
                return i
        }
        return 0
    }

    function openSortMenu() {
        libraryOpen = false
        filtersOpen = false
        sortEntries = buildSortEntries()
        sortIndex = Math.max(0, sortEntries.findIndex(function (entry) {
            return entry.value === currentSortBy()
        }))
        sortOpen = true
        Qt.callLater(function () {
            InputKeys.focus(sortList)
        })
    }

    function openFilterMenu() {
        libraryOpen = false
        sortOpen = false
        filterEntries = buildFilterEntries()
        filterIndex = firstActionableFilterIndex(filterEntries)
        filtersOpen = true
        Qt.callLater(function () {
            InputKeys.focus(filterList)
        })
    }

    function openLibraryMenu() {
        if (root.isFixedBrowseView || libraryCount() <= 0)
            return
        sortOpen = false
        filtersOpen = false
        libraryIndex = Math.max(0, currentLibraryModelIndex())
        libraryEntries = buildLibraryEntries()
        libraryOpen = true
        Qt.callLater(function () {
            InputKeys.focus(libraryList)
        })
    }

    function closeMenus() {
        libraryOpen = false
        sortOpen = false
        filtersOpen = false
        InputKeys.focus(grid)
    }

    function hasShell() {
        return shell !== null && shell !== undefined
    }

    function currentMediaItem() {
        if (grid.currentIndex < 0 || !Browse.items)
            return ({})
        return Browse.items.get(grid.currentIndex) || ({})
    }

    function typeAheadTitle(index) {
        if (!Browse.items || index < 0 || index >= Browse.items.count)
            return ""
        const item = Browse.items.get(index) || ({})
        return String(item.title || item.displayTitle || item.seriesName || "").toLocaleLowerCase()
    }

    function selectTypeAheadMatch(query, includeCurrent) {
        const count = Browse.items ? Browse.items.count : 0
        if (count <= 0 || query.length <= 0)
            return false
        const start = Math.max(0, grid.currentIndex)
        const firstOffset = includeCurrent ? 0 : 1
        for (let offset = firstOffset; offset < firstOffset + count; ++offset) {
            const index = (start + offset) % count
            if (typeAheadTitle(index).indexOf(query) === 0) {
                grid.currentIndex = index
                grid.ensureCurrentVisible()
                grid.requestMoreIfNeeded()
                return true
            }
        }
        for (let offset = firstOffset; offset < firstOffset + count; ++offset) {
            const index = (start + offset) % count
            if (typeAheadTitle(index).indexOf(query) >= 0) {
                grid.currentIndex = index
                grid.ensureCurrentVisible()
                grid.requestMoreIfNeeded()
                return true
            }
        }
        return false
    }

    function typeAhead(text) {
        if (libraryOpen || sortOpen || filtersOpen || !grid.activeFocus || !text || text.length <= 0)
            return false
        const character = String(text).toLocaleLowerCase()
        const code = character.charCodeAt(0)
        if (code < 32 || code === 127 || character === "/" || (character === " " && typeAheadBuffer.length <= 0))
            return false
        const previous = typeAheadBuffer
        const repeatedSingle = previous.length === 1 && previous === character
        const candidate = repeatedSingle ? character : previous + character
        typeAheadBuffer = candidate
        if (!selectTypeAheadMatch(candidate, previous.length > 0 && !repeatedSingle) && candidate.length > 1) {
            typeAheadBuffer = character
            selectTypeAheadMatch(character, false)
        }
        typeAheadReset.restart()
        return true
    }

    function activateLibraryIndex(index) {
        if (index < 0 || index >= libraryCount())
            return
        libraryOpen = false
        sortOpen = false
        filtersOpen = false
        savedIndex = 0
        gridReveal.reset()
        App.openLibrary(index)
        if (hasShell())
            shell.replaceRoute("libraryGrid", {
                                   libraryId: String(Libraries.get(index).libraryId || ""),
                                   focusIndex: 0
                               })
        InputKeys.focus(grid)
    }

    function activateSortEntry(entry) {
        if (!entry)
            return
        savedIndex = 0
        gridReveal.reset()
        if (String(entry.value).indexOf("order:") === 0)
            Browse.setSort(currentSortBy(), String(entry.value).split(":")[1])
        else
            Browse.setSort(entry.value, currentSortOrder())
        sortEntries = buildSortEntries()
    }

    function activateFilterEntry(entry) {
        if (!entry)
            return
        if (entry.kind === "category") {
            expandedFilterSection = expandedFilterSection === entry.sectionName ? "" : entry.sectionName
            filterEntries = buildFilterEntries()
            filterIndex = filterEntries.findIndex(function (candidate) {
                return candidate.kind === "category" && candidate.sectionName === entry.sectionName
            })
            return
        }
        if (entry.section)
            return
        savedIndex = 0
        gridReveal.reset()
        if (entry.kind === "list")
            Browse.setQueryListValue(entry.key, entry.value, !entry.checked)
        else if (entry.kind === "bool")
            Browse.setQueryValue(entry.key, entry.checked ? null : true)
        else if (entry.kind === "nullableBool")
            Browse.setQueryValue(entry.key, entry.checked ? null : entry.value)
        filterEntries = buildFilterEntries()
    }

    function focusToolbar() {
        InputKeys.focus(libraryButton)
    }

    function toolbarButtons() {
        return [libraryButton, viewButton, sortButton, filterButton, clearFiltersButton].filter(function (button) {
            return button.visible
        })
    }

    function toggleViewMode() {
        listMode = !listMode
    }

    // Test and benchmark hook: page the view down by one screen and say
    // whether there was anywhere to go. Driving this directly rather than
    // through key handling means an automated run covers the same distance
    // every time whatever happens to hold focus, and the false at the end is
    // how it knows it has reached the bottom.
    // Test and benchmark hook: what an automated run needs to know about the
    // view without reaching into it -- whether the library has arrived, how
    // far there is to scroll, and which layout is showing.
    function viewState(): var {
        return {
            "count": grid.count,
            "contentHeight": grid.contentHeight,
            "height": grid.height,
            "contentY": grid.contentY,
            "listMode": root.listMode
        }
    }

    function scrollPageDown(): bool {
        const maximum = Math.max(0, grid.contentHeight - grid.height)
        if (maximum <= 0 || grid.contentY >= maximum - 1)
            return false
        grid.contentY = Math.min(maximum, grid.contentY + grid.height)
        grid.requestMoreIfNeeded()
        return true
    }

    // The preview controls stay resident. Only their bound item changes, so
    // title and synopsis update immediately while artwork decodes separately.
    readonly property var paneItem: {
        const index = grid.currentIndex
        if (!listMode || !Browse.items || index < 0 || index >= Browse.items.count)
        return null
        return Browse.items.get(index)
    }

    onListModeChanged: {
        touchPreviewItemId = ""
        Qt.callLater(function () {
            grid.forceLayout()
            grid.ensureCurrentVisible()
            grid.requestMoreIfNeeded()
        })
    }

    // Kodi-style media info for the focused item, formatted natively in one
    // call so scrolling never iterates stream lists from JS.
    readonly property var mediaInfo: {
        if (grid.currentIndex < 0 || grid.count <= 0)
        return null
        const smart = String(Settings.values["audio/trackMode"] || "Default") === "Smart"
        return Browse.mediaInfoFor(grid.currentIndex, smart ? String(Settings.values["subtitles/language"] || "") : "")
    }

    Connections {
        target: Browse
        function onChanged() {
            if (root.filtersOpen)
                root.filterEntries = root.buildFilterEntries()
            if (root.sortOpen)
                root.sortEntries = root.buildSortEntries()
        }
    }

    AtomicViewReveal {
        id: gridReveal

        view: grid
        latencyMonitor: InputLatency
        transitionToken: root.uiTransitionToken
        enabled: Browse.items.count > 0
        firstIndex: Math.min(grid.count - 1, Math.max(0, Math.floor(grid.contentY / grid.cellHeight) * root.columns))
        lastIndex: Math.min(grid.count - 1, Math.max(firstIndex, Math.ceil((grid.contentY + grid.height)
                                                                           / grid.cellHeight) * root.columns - 1))
        onDelegatesReadyChanged: if (delegatesReady)
        grid.requestMoreIfNeeded()
    }

    function activateCurrent() {
        openDetailsAt(grid.currentIndex)
    }

    function activatePointerIndex(index, button) {
        if (index < 0 || index >= grid.count)
            return
        const item = Browse.items.get(index)
        if (button === Qt.RightButton && root.shell) {
            root.shell.openItemMenu(item, grid.itemAtIndex(index))
            return
        }
        const itemId = String(item.movieId || "")
        const selected = index === grid.currentIndex && (!Metrics.coarsePointer || (itemId.length > 0
                                                                                    && touchPreviewItemId === itemId))
        if (!listMode || selected) {
            openDetailsAt(index)
            return
        }
        touchPreviewItemId = Metrics.coarsePointer ? itemId : ""
        // List selection feeds the bio pane, but is not global navigation
        // focus. Preserve the viewport even for a partly visible tapped row.
        const contentY = grid.contentY
        clearPendingPointerNavigation()
        navigationFocusVisible = true
        grid.currentIndex = index
        InputKeys.focus(grid)
        grid.contentY = contentY
    }

    function openDetailsAt(index) {
        if (index < 0 || index >= grid.count)
            return
        savedIndex = index
        const item = Browse.items ? (Browse.items.get(index) || ({})) : ({});
        // browseRoute is empty because this page already is the browse route:
        // playing a container here should not push another one on top.
        ItemActivation.open(item, {
                                "source": "movies",
                                "returnRoute": "libraryGrid",
                                "browseRoute": ""
                            }, App, hasShell() ? shell : null, Browse.items, index)
    }

    function currentCard() {
        return grid.currentItem
    }

    function routeKey(key, phase, repeat) {
        if (phase === "release" && InputKeys.isDirection(key))
            return grid.routeKey(key, phase, repeat)
        if (phase !== "release" && InputKeys.isDirection(key) && grid.activeFocus && pointerNavigationPending)
            return applyPendingPointerNavigation()
        if (libraryList && libraryList.activeFocus)
            return libraryList.routeKey(key, phase, repeat)
        if (sortList && sortList.activeFocus)
            return sortList.routeKey(key, phase, repeat)
        if (filterList && filterList.activeFocus)
            return filterList.routeKey(key, phase, repeat)

        const toolbar = toolbarButtons()
        const toolbarIndex = toolbar.findIndex(function (button) {
            return button.activeFocus
        })
        if (toolbarIndex >= 0) {
            if (key === Qt.Key_Up) {
                if (hasShell())
                    shell.focusNavBar()
                return true
            }
            if (key === Qt.Key_Down) {
                InputKeys.focus(grid)
                return true
            }
            if (key === Qt.Key_Left || key === Qt.Key_Right) {
                const next = toolbarIndex + (key === Qt.Key_Right ? 1 : -1)
                if (next >= 0 && next < toolbar.length)
                    InputKeys.focus(toolbar[next])
                return true
            }
            return false
        }
        return grid.count > 0 && grid.routeKey(key, phase, repeat)
    }

    function clearPendingPointerNavigation() {
        pointerNavigationPending = false
        pendingScrollController = null
    }

    function beginPointerNavigation(controller) {
        if (controller)
            pendingScrollController = controller
        pointerNavigationPending = true
        navigationFocusVisible = false
    }

    function applyPendingPointerNavigation() {
        if (pendingScrollController && pendingScrollController.stopScrolling)
            pendingScrollController.stopScrolling()
        grid.cancelFlick()
        grid.forceLayout()
        const visibleIndex = grid.topLeftVisibleIndex()
        if (visibleIndex < 0) {
            clearPendingPointerNavigation()
            navigationFocusVisible = true
            return true
        }
        const contentX = grid.contentX
        const contentY = grid.contentY
        InputKeys.focusIndexWithoutScrolling(grid, visibleIndex)
        grid.contentX = contentX
        grid.contentY = contentY
        navigationFocusVisible = true
        clearPendingPointerNavigation()
        return true
    }

    function activate() {
        if (libraryList && libraryList.activeFocus)
            libraryList.activate()
        else if (sortList && sortList.activeFocus)
            sortList.activate()
        else if (filterList && filterList.activeFocus)
            filterList.activate()
        else if (libraryButton.activeFocus)
            openLibraryMenu()
        else if (viewButton.activeFocus)
            toggleViewMode()
        else if (sortButton.activeFocus)
            openSortMenu()
        else if (filterButton.activeFocus)
            openFilterMenu()
        else if (clearFiltersButton.activeFocus)
            Browse.clearFilters()
        else
            grid.activate()
    }

    function longPress() {
        if (!grid.activeFocus || grid.currentIndex < 0 || !hasShell())
            return false
        return shell.openItemMenu(Browse.items.get(grid.currentIndex) || ({}), currentCard(), {
                                      "deferBackdropDismissal": true
                                  })
    }

    function back() {
        if (typeAheadBuffer.length > 0) {
            typeAheadBuffer = ""
            typeAheadReset.stop()
            return true
        }
        if (libraryOpen || sortOpen || filtersOpen) {
            closeMenus()
            return true
        }
        return false
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: Metrics.pageMarginPx
        anchors.rightMargin: Metrics.pageMarginPx
        anchors.topMargin: root.contentTopMargin
        anchors.bottomMargin: 0
        spacing: 12
        RowLayout {
            Layout.fillWidth: true
            Layout.rightMargin: -Metrics.pageMarginPx + Metrics.scaled(14)
            spacing: 10

            FocusScope {
                id: libraryButton
                Layout.fillWidth: true
                Layout.preferredHeight: Metrics.scaled(44)
                focus: true

                Rectangle {
                    anchors.left: titleRow.left
                    anchors.right: titleRow.right
                    anchors.verticalCenter: titleRow.verticalCenter
                    height: Metrics.scaled(36)
                    radius: Theme.radiusSmall
                    color: libraryButton.activeFocus || root.libraryOpen ? Theme.accentPanel : "transparent"
                    border.width: libraryButton.activeFocus ? 2 : 0
                    border.color: Theme.textPrimary
                }

                Row {
                    id: titleRow
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 4
                    anchors.rightMargin: 8
                    spacing: 6
                    width: Math.min(titleText.implicitWidth + 30, Math.max(160, parent.width - headerDetailText.width
                                                                           - 32))

                    AppText {
                        id: titleText
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(0, titleRow.width - 30)
                        text: Browse.title.length > 0 ? Browse.title : "Library"
                        font.pixelSize: Metrics.bodySizePx + 4
                        font.weight: Font.DemiBold
                        maximumLineCount: 1
                        elide: Text.ElideRight
                    }

                    MaterialIcon {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !root.isFixedBrowseView
                        name: root.libraryOpen ? "expand_less" : "expand_more"
                        iconSize: 24
                        iconColor: root.libraryOpen || libraryButton.activeFocus ? Theme.textPrimary :
                                                                                   Theme.textSecondary
                    }
                }

                SecondaryText {
                    id: headerDetailText
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.headerDetail()
                    color: Theme.textMuted
                    font.pixelSize: Metrics.metaSizePx
                    maximumLineCount: 1
                    elide: Text.ElideRight
                    width: Math.min(520, Math.max(0, parent.width * 0.52))
                }

                BusySpinner {
                    anchors.right: headerDetailText.left
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: 28
                    height: 28
                    running: root.browseLoading
                    visible: running
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: root.openLibraryMenu()
                }
            }

            ToolbarButton {
                id: viewButton
                iconName: root.listMode ? "view_list" : "grid_view"
                label: root.listMode ? "List" : "Posters"
                onActivated: root.toggleViewMode()
            }

            ToolbarButton {
                id: sortButton
                visible: !root.isFixedBrowseView
                iconName: root.currentSortOrder() === "Descending" ? "south" : "north"
                label: root.currentSortLabel()
                checked: root.sortOpen
                onActivated: root.openSortMenu()
            }

            ToolbarButton {
                id: filterButton
                visible: !root.isFixedBrowseView
                iconName: "filter_list"
                label: "Filter"
                checked: root.filtersOpen
                badge: root.activeFilterCount > 0
                onActivated: root.filtersOpen ? root.closeMenus() : root.openFilterMenu()
            }

            ToolbarButton {
                id: clearFiltersButton
                iconName: "close"
                label: "Clear"
                visible: !root.isFixedBrowseView && root.activeFilterCount > 0 && root.lane !== "compact"
                onActivated: {
                    root.savedIndex = 0
                    gridReveal.reset()
                    Browse.clearFilters()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.rightMargin: -Metrics.pageMarginPx
            Layout.fillHeight: true
            Layout.topMargin: root.listMode ? 0 : Metrics.scaled(6)
            Layout.bottomMargin: mediaInfoBar.visible ? mediaInfoBar.height + Metrics.scaled(10) : 0
            spacing: root.listMode ? Metrics.sectionGapPx : 0

            // List mode's detail controls stay resident and rebind to the
            // current item; no adjacent or off-screen preview panes exist.
            Item {
                id: listPane
                // The pane is a companion column, so it only exists where there is
                // room for one beside the list.
                visible: root.listMode && root.lane !== "compact"
                Layout.preferredWidth: Math.round(root.width * (root.lane === "wide" ? 0.28 : 0.36))
                Layout.fillHeight: true

                ImageCard {
                    id: panePoster
                    anchors.top: parent.top
                    anchors.horizontalCenter: parent.horizontalCenter
                    height: Math.round(Math.min(parent.width * root.artworkAspect, parent.height * (root.largeZoom
                                                                                                    ? 0.64 : 0.56)))
                    width: Math.round(height / root.artworkAspect)
                    imageUrl: root.paneItem && root.paneItem.movieId ? Art.url(root.paneItem, root.squareArtwork
                                                                               ? "square" : "poster") : ""
                    fallbackText: String(root.paneItem && root.paneItem.title || "")
                }

                Item {
                    id: paneHeading
                    readonly property bool metaInline: paneMeta.text.length > 0 && paneTitle.implicitWidth
                                                       + paneMeta.implicitWidth + Metrics.scaled(10) <= width
                    anchors.top: panePoster.bottom
                    anchors.topMargin: Metrics.scaled(root.smallZoom ? 30 : 22)
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: metaInline ? Math.max(paneTitle.contentHeight, paneMeta.implicitHeight) :
                                         paneTitle.contentHeight + (paneMeta.text.length > 0 ? Metrics.scaled(4)
                                                                                               + paneMeta.implicitHeight :
                                                                                               0)

                    TextMetrics {
                        id: paneTitleMetrics
                        font: paneTitle.font
                        text: paneTitle.text
                    }

                    TextMetrics {
                        id: paneMetaMetrics
                        font: paneMeta.font
                        text: paneMeta.text
                    }

                    AppText {
                        id: paneTitle
                        anchors.top: parent.top
                        anchors.left: parent.left
                        width: paneHeading.metaInline ? implicitWidth : parent.width
                        text: String(root.paneItem && root.paneItem.title || "")
                        font.pixelSize: Metrics.bodySizePx + 4
                        font.weight: Font.DemiBold
                        maximumLineCount: 2
                        wrapMode: Text.Wrap
                        elide: Text.ElideRight
                    }

                    SecondaryText {
                        id: paneMeta
                        anchors.left: paneHeading.metaInline ? paneTitle.right : parent.left
                        anchors.leftMargin: paneHeading.metaInline ? Metrics.scaled(10) : 0
                        y: paneHeading.metaInline ? Math.round(paneTitle.baselineOffset
                                                               + paneTitleMetrics.tightBoundingRect.y
                                                               + paneTitleMetrics.tightBoundingRect.height / 2
                                                               - paneMeta.baselineOffset
                                                               - paneMetaMetrics.tightBoundingRect.y
                                                               - paneMetaMetrics.tightBoundingRect.height / 2) :
                                                    paneTitle.contentHeight + Metrics.scaled(4)
                        text: String(root.paneItem && root.paneItem.subtitle || "")
                        visible: text.length > 0
                        color: Theme.textMuted
                        font.pixelSize: Metrics.metaSizePx
                        maximumLineCount: 1
                        elide: Text.ElideRight
                    }
                }

                AppText {
                    anchors.top: paneHeading.bottom
                    anchors.topMargin: Metrics.scaled(10)
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    visible: text.length > 0
                    text: String(root.paneItem && root.paneItem.overview || "")
                    color: Theme.textSecondary
                    font.pixelSize: Metrics.bodySizePx
                    lineHeight: 1.2
                    wrapMode: Text.Wrap
                    elide: Text.ElideRight
                }
            }

            NavGrid {
                id: grid
                property int artworkWindowRevision: 0
                readonly property int memoryMiB: NativeWindow.systemMemoryBytes > 0 ? Math.round(
                                                                                          NativeWindow.systemMemoryBytes
                                                                                          / 1048576) : 2048
                readonly property int artworkMarginRows: Platform.isTV ? (memoryMiB >= 3000 ? 8 : memoryMiB >= 1800 ? 5 : 3) :
                                                                         12
                readonly property int focusPadding: Math.max(2, Metrics.scaled(2))
                property int geometryAnchorIndex: -1
                holdTraversalSeconds: root.listMode && count > 200 ? 3.5 : 5
                holdSpeedMultiplier: root.listMode ? 1 : 0.5
                Layout.fillWidth: true
                Layout.fillHeight: true
                focus: true
                clip: true
                keyNavigationEnabled: false
                reuseItems: true
                boundsBehavior: Flickable.StopAtBounds
                model: Browse.items
                leftMargin: focusPadding
                rightMargin: focusPadding + Math.max(Metrics.scaled(10), 10) + Metrics.scaled(6)
                // Each cell carries its own trailing gap (delegates are
                // cellWidth - gapPx), so the track must not be charged for the
                // gaps a second time: divide the whole run evenly and let the
                // posters grow into the space the double count used to waste.
                cellWidth: Math.floor((width - leftMargin - rightMargin) / columns)
                cellHeight: root.listMode ? Metrics.scaled(root.largeZoom ? 60 : 54) : cellWidth * root.artworkAspect + Metrics.scaled(
                                                64)

                cacheBuffer: gridReveal.delegatesReady ? cellHeight * artworkMarginRows : 0
                Component.onCompleted: {
                    restoreIndex()
                    requestMoreIfNeeded()
                    gridReveal.reset()
                }
                onCellWidthChanged: scheduleGeometryRelayout()
                onCellHeightChanged: scheduleGeometryRelayout()
                onCountChanged: {
                    if (count <= 0)
                    currentIndex = -1
                    else if (currentIndex < 0 || currentIndex >= count)
                    restoreIndex()
                    requestMoreIfNeeded()
                }
                onContentYChanged: {
                    requestPageIfNeeded()
                    artworkWindowDebounce.restart()
                    alphabetFeedback.restart()
                }
                onCurrentIndexChanged: {
                    if (currentIndex >= 0)
                    root.savedIndex = currentIndex
                    alphabetFeedback.restart()
                    requestPageIfNeeded()
                    routeCheckpoint.restart()
                }

                FastWheelHandler {
                    id: gridWheelHandler
                    onScrolled: root.beginPointerNavigation(gridWheelHandler)
                    flickable: grid
                }
                onDraggingChanged: if (dragging)
                root.beginPointerNavigation(null)
                ListScrollBar {
                    id: libraryScrollBar
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    width: Math.ceil(Math.max(Metrics.scaled(10), 10) * 3)
                    // Above the delegate MouseArea below, which fills the view
                    // and would otherwise grab every press over the bar.
                    z: 4
                    flickable: grid
                    // The magic remote points and drags, so a television is
                    // not on its own a reason to make the bar inert.
                    interactive: Platform.hasPointer
                    minimumSize: 0.04
                    onScrolled: root.beginPointerNavigation(null)
                }
                onEdgeUp: root.focusToolbar()
                onAccepted: root.activateCurrent()
                onHoldStarted: Browse.prefetchNextPage()

                function restoreIndex() {
                    currentIndex = count > 0 ? Math.max(0, Math.min(root.savedIndex, count - 1)) : -1
                    ensureCurrentVisible()
                }
                function scheduleGeometryRelayout() {
                    if (!geometryRelayout.running)
                        geometryAnchorIndex = topLeftVisibleIndex()
                    geometryRelayout.restart()
                }

                function applyGeometryRelayout() {
                    forceLayout()
                    const anchor = count > 0 ? Math.max(0, Math.min(geometryAnchorIndex, count - 1)) : -1
                    if (anchor === 0)
                        positionViewAtBeginning()
                    else if (anchor > 0)
                        positionViewAtIndex(anchor, GridView.Beginning)
                    geometryAnchorIndex = -1
                    requestMoreIfNeeded()
                }

                Timer {
                    id: geometryRelayout
                    interval: 0
                    onTriggered: grid.applyGeometryRelayout()
                }

                function ensureCurrentVisible() {
                    if (currentIndex >= 0)
                        positionViewAtIndex(currentIndex, GridView.Contain)
                }

                function lastLikelyVisibleIndex() {
                    if (count <= 0 || cellHeight <= 0 || columns <= 0)
                        return -1
                    const firstRow = Math.max(0, Math.floor(contentY / cellHeight))
                    const visibleRows = Math.ceil(height / cellHeight) + 3
                    return Math.min(count - 1, (firstRow + visibleRows) * columns - 1)
                }

                function firstLikelyVisibleIndex() {
                    if (count <= 0 || cellHeight <= 0 || columns <= 0)
                        return -1
                    return Math.min(count - 1, Math.max(0, Math.floor(contentY / cellHeight) * columns))
                }

                function requestMoreIfNeeded() {
                    if (count <= 0 || !gridReveal.delegatesReady)
                        return
                    const visibleHead = firstLikelyVisibleIndex()
                    const visibleTail = Math.max(currentIndex, lastLikelyVisibleIndex())
                    Browse.prefetchVisibleRange(visibleHead, visibleTail)
                }

                function artworkIndexResident(index) {
                    const revision = artworkWindowRevision
                    if (revision < 0 || index < 0 || count <= 0)
                        return false
                    const margin = artworkMarginRows * Math.max(1, columns)
                    const first = Math.max(0, firstLikelyVisibleIndex() - margin)
                    const last = Math.min(count - 1, lastLikelyVisibleIndex() + margin)
                    return index >= first && index <= last
                }

                function requestPageIfNeeded() {
                    if (count <= 0)
                        return
                    Browse.prefetchPageForIndex(Math.max(currentIndex, lastLikelyVisibleIndex()))
                }

                Timer {
                    id: artworkWindowDebounce
                    interval: 60
                    repeat: false
                    onTriggered: {
                        grid.artworkWindowRevision++
                        grid.requestMoreIfNeeded()
                    }
                }

                Timer {
                    id: routeCheckpoint
                    interval: 250
                    repeat: false
                    onTriggered: if (root.shell)
                    Router.checkpoint({
                                          libraryId: Browse.libraryId,
                                          focusIndex: Math.max(0, grid.currentIndex)
                                      })
                }

                Timer {
                    id: alphabetFeedback
                    interval: 700
                    repeat: false
                }

                // Scrub letter callout: rounded on its left, top and bottom, with
                // no right edge — the top-right and bottom-right shoulders funnel
                // into the scroll bar so the tag reads as emanating from it.
                Shape {
                    id: alphabetCallout

                    readonly property real stroke: Math.max(2, Metrics.scaled(2))
                    readonly property real inset: stroke / 2
                    readonly property real cornerRadius: Metrics.scaled(12)
                    readonly property real neckWidth: Metrics.scaled(16)
                    readonly property real mouthHalf: Metrics.scaled(13)
                    readonly property real bodyRight: width - neckWidth
                    readonly property real midY: height / 2
                    readonly property real desiredY: libraryScrollBar.handleCenterY - height / 2

                    anchors.right: libraryScrollBar.right
                    anchors.rightMargin: libraryScrollBar.visualWidth
                    y: Math.max(0, Math.min(grid.height - height, desiredY))
                    width: Metrics.scaled(52) + neckWidth
                    height: Metrics.scaled(48)
                    // Directional navigation scrolls the view without touching
                    // the bar, so the bar says where it has gone: the callout
                    // reads the same whether a remote walked the grid there or
                    // a pointer dragged the handle.
                    opacity: alphabetFeedback.running || libraryScrollBar.pressed ? 1 : 0
                    visible: opacity > 0
                    preferredRendererType: Shape.CurveRenderer
                    z: 21

                    // The stroke runs mouth -> top -> left -> bottom -> mouth and
                    // stops there; only the fill closes the contour across the
                    // bar, which is what leaves the right edge open.
                    ShapePath {
                        fillColor: Theme.accentPanel
                        strokeColor: Theme.accent
                        strokeWidth: alphabetCallout.stroke
                        capStyle: ShapePath.FlatCap
                        joinStyle: ShapePath.RoundJoin
                        startX: alphabetCallout.width
                        startY: alphabetCallout.midY - alphabetCallout.mouthHalf

                        PathCubic {
                            control1X: alphabetCallout.width - alphabetCallout.neckWidth * 0.35
                            control1Y: alphabetCallout.midY - alphabetCallout.mouthHalf
                            control2X: alphabetCallout.bodyRight + alphabetCallout.neckWidth * 0.55
                            control2Y: alphabetCallout.inset
                            x: alphabetCallout.bodyRight
                            y: alphabetCallout.inset
                        }
                        PathLine {
                            x: alphabetCallout.cornerRadius + alphabetCallout.inset
                            y: alphabetCallout.inset
                        }
                        PathArc {
                            radiusX: alphabetCallout.cornerRadius
                            radiusY: alphabetCallout.cornerRadius
                            direction: PathArc.Counterclockwise
                            x: alphabetCallout.inset
                            y: alphabetCallout.cornerRadius + alphabetCallout.inset
                        }
                        PathLine {
                            x: alphabetCallout.inset
                            y: alphabetCallout.height - alphabetCallout.cornerRadius - alphabetCallout.inset
                        }
                        PathArc {
                            radiusX: alphabetCallout.cornerRadius
                            radiusY: alphabetCallout.cornerRadius
                            direction: PathArc.Counterclockwise
                            x: alphabetCallout.cornerRadius + alphabetCallout.inset
                            y: alphabetCallout.height - alphabetCallout.inset
                        }
                        PathLine {
                            x: alphabetCallout.bodyRight
                            y: alphabetCallout.height - alphabetCallout.inset
                        }
                        PathCubic {
                            control1X: alphabetCallout.bodyRight + alphabetCallout.neckWidth * 0.55
                            control1Y: alphabetCallout.height - alphabetCallout.inset
                            control2X: alphabetCallout.width - alphabetCallout.neckWidth * 0.35
                            control2Y: alphabetCallout.midY + alphabetCallout.mouthHalf
                            x: alphabetCallout.width
                            y: alphabetCallout.midY + alphabetCallout.mouthHalf
                        }
                    }

                    AppText {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.horizontalCenterOffset: -alphabetCallout.neckWidth / 2
                        width: alphabetCallout.bodyRight - Metrics.scaled(8)
                        height: alphabetCallout.height - Metrics.scaled(8)
                        text: root.currentAlphabetLabel
                        font.pixelSize: Metrics.scaled(26)
                        fontSizeMode: Text.Fit
                        minimumPixelSize: Metrics.scaled(8)
                        elide: Text.ElideRight
                        font.weight: Font.Bold
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: Theme.textPrimary
                    }

                    Behavior on opacity {
                        NumberAnimation {
                            duration: Theme.reducedMotion ? 0 : 100
                        }
                    }
                }

                delegate: root.listMode ? listRowComponent : posterCardComponent

                Component {
                    id: posterCardComponent

                    MediaItemCard {
                        id: gridDelegate

                        required property int index

                        width: grid.cellWidth - Metrics.gapPx
                        height: grid.cellHeight
                        shell: root.shell
                        kind: root.squareArtwork ? "square" : "poster"
                        preferEpisodeTitle: false
                        useSeriesPoster: true
                        focused: Metrics.keyboardFocusActive && root.navigationFocusVisible && grid.activeFocus
                                 && gridDelegate.GridView.isCurrentItem
                        artworkVisible: true
                        artworkEnabled: grid.artworkIndexResident(index)

                        Component.onCompleted: gridReveal.schedule()
                        onArtworkReadyChanged: gridReveal.schedule()
                    }
                }

                Component {
                    id: listRowComponent

                    Item {
                        id: listRow

                        required property int index
                        required property string displayTitle
                        required property string displaySubtitle

                        // Consumed by the shared highlight delegate.
                        readonly property real focusOutlineHeight: height - Metrics.scaled(6)

                        width: grid.cellWidth - Metrics.gapPx
                        height: grid.cellHeight

                        AppText {
                            anchors.left: parent.left
                            anchors.leftMargin: Metrics.scaled(14)
                            anchors.right: rowSubtitle.left
                            anchors.rightMargin: Metrics.scaled(16)
                            anchors.verticalCenter: parent.verticalCenter
                            text: listRow.displayTitle
                            font.pixelSize: Metrics.bodySizePx + Metrics.scaled(2)
                            font.weight: listRow.GridView.isCurrentItem ? Font.DemiBold : Font.Medium
                            color: listRow.GridView.isCurrentItem ? Theme.textPrimary : Theme.textSecondary
                            maximumLineCount: 1
                            elide: Text.ElideRight
                        }

                        SecondaryText {
                            id: rowSubtitle
                            anchors.right: parent.right
                            anchors.rightMargin: Metrics.scaled(14)
                            anchors.verticalCenter: parent.verticalCenter
                            text: listRow.displaySubtitle
                            color: Theme.textMuted
                            font.pixelSize: Metrics.metaSizePx + Metrics.scaled(1)
                            maximumLineCount: 1
                        }

                        Component.onCompleted: gridReveal.schedule()
                    }
                }
                highlightFollowsCurrentItem: true
                highlight: Item {
                    z: 2

                    Rectangle {
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: grid.currentItem ? grid.currentItem.focusOutlineHeight : parent.height
                        color: "transparent"
                        radius: Math.max(0, Theme.radiusMedium - Theme.focusBorderWidth)
                        border.width: Theme.focusBorderWidth
                        border.color: root.navigationFocusVisible && ((Metrics.keyboardFocusActive && grid.activeFocus)
                                                                      || (root.listMode && Metrics.coarsePointer
                                                                          && root.touchPreviewItemId.length > 0
                                                                          && root.touchPreviewItemId === String(
                                                                              root.paneItem ? root.paneItem.movieId :
                                                                                              ""))) ? Theme.accent :
                                                                                                      "transparent"
                    }
                }

                GridPointerArea {
                    view: grid
                    onActivated: (index, button) => root.activatePointerIndex(index, button)
                    onContextRequested: index => {
                        if (root.shell)
                            root.shell.openItemMenu(Browse.items.get(index), grid.itemAtIndex(index), {
                                                        "deferBackdropDismissal": true
                                                    })
                    }
                    onContextGestureEnded: if (root.shell)
                    root.shell.finishItemMenuOpeningGesture()
                }
            }
        }
    }

    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: Metrics.pageMarginPx
        anchors.topMargin: root.contentTopMargin + 54
        width: typeAheadText.implicitWidth + 30
        height: 40
        radius: height / 2
        visible: root.typeAheadBuffer.length > 0
        color: Theme.floatingPanel
        border.width: 1
        border.color: Theme.accent
        z: 30

        AppText {
            id: typeAheadText
            anchors.centerIn: parent
            text: root.typeAheadBuffer
            font.pixelSize: Metrics.bodySizePx
            font.weight: Font.DemiBold
        }
    }

    Timer {
        id: typeAheadReset
        interval: 1250
        repeat: false
        onTriggered: root.typeAheadBuffer = ""
    }

    // Kodi-style single-line media info for the focused item.
    TechnicalDetailsBar {
        id: mediaInfoBar
        visible: root.mediaInfo !== null && Theme.technicalMetadataMode === "Always" && hasContent
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 0
        anchors.right: parent.right
        anchors.rightMargin: 0
        width: root.listMode ? grid.cellWidth - Metrics.gapPx : Math.max(0, grid.width - grid.leftMargin
                                                                         - grid.rightMargin)

        height: implicitHeight
        info: root.mediaInfo
        z: 18
    }
    MouseArea {
        anchors.fill: parent
        visible: root.libraryOpen || root.sortOpen || root.filtersOpen
        z: 19
        onClicked: root.closeMenus()
    }

    LazyMenuPanel {
        id: libraryPanel
        open: root.libraryOpen && !root.isFixedBrowseView
        anchors.left: parent.left
        anchors.topMargin: root.contentTopMargin + 52
        anchors.leftMargin: Metrics.pageMarginPx
        width: Metrics.menuPanelWidth(root.width, 360)
        maximumHeight: Metrics.scaled(420)
        z: 22
        model: root.libraryEntries
        currentIndex: root.libraryIndex
        edgeEscapeItem: libraryButton
        checkedFor: function (entry) {
            return entry && String(entry.libraryId) === String(Browse.libraryId || "")
        }
        onCurrentIndexChanged: root.libraryIndex = currentIndex
        onDismissed: root.closeMenus()
        onAccepted: entry => root.activateLibraryIndex(entry.index)
    }

    LazyMenuPanel {
        id: sortPanel
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: root.contentTopMargin + 52
        anchors.rightMargin: Metrics.pageMarginPx
        width: Metrics.menuPanelWidth(root.width, 320)
        open: root.sortOpen
        maximumHeight: Metrics.scaled(430)
        z: 20
        model: root.sortEntries
        currentIndex: root.sortIndex
        edgeEscapeItem: sortButton
        checkedFor: function (entry) {
            return String(entry && entry.value || "").indexOf("order:") === 0 ? String(entry.value).split(":")[1] === root.currentSortOrder() :
                                                                                entry && entry.value
                                                                                === root.currentSortBy()
        }
        onCurrentIndexChanged: root.sortIndex = currentIndex
        onDismissed: root.closeMenus()
        onAccepted: entry => root.activateSortEntry(entry)
    }

    LazyMenuPanel {
        id: filterPanel
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: root.contentTopMargin + 52
        anchors.rightMargin: Metrics.pageMarginPx
        width: Metrics.menuPanelWidth(root.width, 380)
        open: root.filtersOpen
        maximumHeight: Math.min(root.height - Metrics.pageMarginPx * 2 - 70, 620)
        z: 21
        model: root.filterEntries
        currentIndex: root.filterIndex
        edgeEscapeItem: filterButton
        title: "Filters"
        selectionStyle: "check"
        resetVisible: true
        resetEnabled: root.activeFilterCount > 0
        onCurrentIndexChanged: root.filterIndex = currentIndex
        onDismissed: root.closeMenus()
        onAccepted: entry => root.activateFilterEntry(entry)
        onReset: {
            root.savedIndex = 0
            Browse.clearFilters()
        }
    }
}
