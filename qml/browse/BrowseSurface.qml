pragma ComponentBehavior: Bound

import QtQuick
import Spool
import "../theme"
import "../primitives"
import "../primitives/ModelAccess.js" as ModelAccess
import "../shell/ItemActivation.js" as ItemActivation

// Browsing cut down to a single column, for places too narrow to hold a page:
// the sheet beside the player now, a phone later.
//
// It is one list all the way down rather than a stack of views. Descending
// swaps the model the list is bound to and remembers where the cursor was, so
// a level costs no construction at all and arrives on the next frame -- which
// is the only way this feels right on a television.
FocusScope {
    id: root

    property string rootTitle: "Add to queue"
    // [{ action, icon, label }] offered on every playable row.
    property var rowActions: []
    // Item id of whatever is playing, so the list can mark it.
    property string currentItemId: ""

    // Where we are. Each level remembers its own cursor so stepping back
    // lands on the row that was used to step forward.
    property var levels: [
        {
            "kind": "root",
            "title": rootTitle,
            "cursor": 0
        }
    ]
    readonly property var level: levels[levels.length - 1]
    readonly property bool atRoot: levels.length <= 1

    signal actionTriggered(var item, string action)
    signal dismissed

    // The destinations at the top: what the home page would show, as places
    // rather than as shelves. Libraries appear through their recent items,
    // which is the row the home page gives them too.
    readonly property var destinations: {
        const places = [
                  {
                      "key": "resume",
                      "title": "Continue Watching",
                      "icon": "play_circle",
                      "model": Home.resumeItems,
                      "kind": "landscape"
                  },
                  {
                      "key": "nextUp",
                      "title": "Next Up",
                      "icon": "skip_next",
                      "model": Home.nextUpItems,
                      "kind": "landscape"
                  }
              ]
        const latest = Home.latestLibraryRows || []
        for (let index = 0; index < latest.length; ++index) {
            const row = latest[index]
            if (!row || !row.model)
                continue
            places.push({
                            "key": "latest" + index,
                            "title": String(row.title || "Recently Added"),
                            "icon": "video_library",
                            "model": row.model,
                            "kind": String(row.kind || "poster") === "landscape" ? "landscape" : "poster"
                        })
        }
        return places
    }

    readonly property bool showingDestinations: level.kind === "root"
    readonly property var activeModel: showingDestinations ? destinations : level.model
    readonly property string activeKind: showingDestinations ? "landscape" : String(level.kind === "children"
                                                                                    ? level.cardKind : "landscape")
    readonly property bool activeBusy: !showingDestinations && level.kind === "children" && Content.detailRowsBusy

    function titleOf(entry) {
        return String((entry && entry.title) || "")
    }

    function push(entry) {
        // Remember where this level was left before covering it.
        const stack = levels.slice()
        stack[stack.length - 1] = Object.assign({}, stack[stack.length - 1], {
                                                    "cursor": listView.currentIndex
                                                })
        stack.push(entry)
        levels = stack
        listView.currentIndex = 0
        Qt.callLater(listView.focusList)
    }

    function pop() {
        if (atRoot)
            return false
        const stack = levels.slice()
        stack.pop()
        levels = stack
        listView.currentIndex = Number(levels[levels.length - 1].cursor || 0)
        // Descending replaced what Content holds; climbing back has to ask
        // for the level above again or the list would show the wrong children.
        reloadChildren()
        Qt.callLater(listView.focusList)
        return true
    }

    function reloadChildren() {
        if (level.kind !== "children")
            return
        Content.loadDetailRows(String(level.itemId || ""), String(level.itemType || ""), String(level.seriesId || ""),
                               String(level.seasonId || ""))
    }

    // Descend into anything with children; anything else is a leaf and the
    // row's own actions are the only thing to do with it.
    function open(index, item) {
        if (showingDestinations) {
            const place = root.destinations[index]
            push({
                     "kind": "model",
                     "title": String(place.title),
                     "model": place.model,
                     "cardKind": String(place.kind),
                     "cursor": 0
                 })
            return
        }
        if (!ItemActivation.hasChildren(item))
            return
        const type = String(item.itemType || "")
        const itemId = String(item.movieId || item.id || "")
        const entry = {
            "kind": "children",
            "title": String(item.title || item.seriesName || ""),
            "itemId": itemId,
            "itemType": type,
            "seriesId": type === "Season" ? String(item.seriesId || "") : "",
            "seasonId": type === "Season" ? itemId : "",
            "cardKind": type === "Series" ? "poster" : "landscape",
            "model": Content.detailSeasons,
            "cursor": 0
        }
        push(entry)
        reloadChildren()
    }

    function routeKey(key, phase, repeat) {
        if (phase === "release")
            return true
        return listView.routeKey(key, phase, repeat)
    }

    function activate() {
        listView.activate()
    }

    function back() {
        if (pop())
            return true
        root.dismissed()
        return true
    }

    function focusList() {
        return listView.focusList()
    }

    Column {
        anchors.fill: parent
        spacing: Metrics.scaled(10)

        Item {
            width: parent.width
            height: Math.max(backButton.height, headingText.implicitHeight)

            IconButton {
                id: backButton
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                visible: !root.atRoot
                focusPolicy: Qt.NoFocus
                chromeless: true
                iconName: "arrow_back"
                accessibleName: "Back"
                onClicked: root.pop()
            }

            AppText {
                id: headingText
                anchors.left: backButton.visible ? backButton.right : parent.left
                anchors.leftMargin: backButton.visible ? Metrics.scaled(6) : 0
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: root.titleOf(root.level)
                font.pixelSize: Metrics.titleSizePx
                font.weight: Font.DemiBold
                maximumLineCount: 1
                elide: Text.ElideRight
            }
        }

        ListBrowseView {
            id: listView

            width: parent.width
            height: parent.height - y
            model: root.activeModel
            cardKind: root.activeKind
            busy: root.activeBusy
            currentItemId: root.currentItemId
            showChevrons: root.showingDestinations
            // A place has nothing to queue; only the things inside it do.
            rowActions: root.showingDestinations ? [] : root.rowActions
            rowTitle: root.showingDestinations ? (item, index) => String((item && item.title) || "") : null
            rowIcon: root.showingDestinations ? (item, index) => String((item && item.icon) || "") : null
            emptyTitle: root.showingDestinations ? "Nothing to browse" : "Nothing here"
            emptyDetail: root.showingDestinations ? "Your libraries have not loaded yet." : ""
            focus: true

            onEdgeUp: root.dismissed()
            onActivated: (index, item) => root.open(index, item)
            onActionTriggered: (index, item, action) => root.actionTriggered(item, action)
        }
    }
}
