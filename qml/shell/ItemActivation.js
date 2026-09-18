.pragma library

// What activating an item means, in one place. Library grids, search
// results, home rows and the player's browse sheet used to each branch on
// itemType themselves, and had drifted: search opened a person's page,
// the library grid did not; search pushed a route after playing a
// container, the grid did not.
//
// This module decides and the caller acts. Keeping the decision a pure
// function of the item is what makes it testable, and what lets a caller
// with no shell -- the sheet over the player -- ask the same question and
// answer it its own way.

const CONTAINER_TYPES = ["Playlist", "Folder", "PhotoAlbum", "MusicArtist"]
const EPISODIC_TYPES = ["Series", "Season"]

function itemType(item) {
    return String((item && item.itemType) || "")
}

// Something whose children are played as a run rather than opened.
function isContainer(item) {
    return CONTAINER_TYPES.indexOf(itemType(item)) >= 0
}

// Something that holds episodes, so it can be drilled into or queued whole.
function isEpisodic(item) {
    return EPISODIC_TYPES.indexOf(itemType(item)) >= 0
}

// True where activating should descend rather than open a details page,
// which is what the compact sheet wants to know.
function hasChildren(item) {
    return isEpisodic(item) || isContainer(item) || itemType(item) === "MusicAlbum" || itemType(item) === "BoxSet"
}

// context: { source, returnRoute, browseRoute }
// browseRoute is where to land after playing a container; empty means the
// caller is already looking at it.
function plan(item, context) {
    const ctx = context || ({})
    const type = itemType(item)
    if (type === "Audio")
        return {
            "action": "playModel",
            "browseRoute": ""
        }
    if (type === "Person")
        return {
            "action": "person",
            "person": {
                "id": String((item && item.movieId) || ""),
                "name": String((item && item.title) || ""),
                "type": "Person"
            }
        }
    if (isContainer(item))
        return {
            "action": "playModel",
            "browseRoute": String(ctx.browseRoute || "")
        }
    return {
        "action": "details",
        "source": type === "MusicAlbum" ? "album" : String(ctx.source || "movies"),
        "returnRoute": String(ctx.returnRoute || "")
    }
}

// App and shell arrive as arguments because a library module has no QML
// context of its own to find singletons in.
function perform(decision, app, shell, model, index) {
    if (!decision)
        return false
    if (decision.action === "person") {
        if (!shell || decision.person.id.length <= 0)
            return false
        shell.openPerson(decision.person)
        return true
    }
    if (decision.action === "playModel") {
        if (!app)
            return false
        app.playFromModel(model, index)
        if (decision.browseRoute.length > 0 && shell)
            shell.pushRoute(decision.browseRoute)
        return true
    }
    if (!shell)
        return false
    shell.openDetailsAt(model, index, decision.source, decision.returnRoute)
    return true
}

function open(item, context, app, shell, model, index) {
    return perform(plan(item, context), app, shell, model, index)
}
