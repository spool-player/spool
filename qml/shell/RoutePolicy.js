.pragma library

function itemIdFor(item) {
    return item ? String(item.movieId || item.itemId || item.id || "") : ""
}

function itemTypeFor(item) {
    return item ? String(item.itemType || item.type || "") : ""
}

function personContext(args) {
    const routeArgs = args || ({})
    return {
        id: String(routeArgs.personId || ""),
        name: String(routeArgs.personName || "Person"),
        role: String(routeArgs.personRole || ""),
        type: String(routeArgs.personType || "Person"),
        imageTag: String(routeArgs.personImageTag || "")
    }
}

function modelCount(model) {
    if (!model)
        return 0
    return model.count !== undefined ? Number(model.count) : model.rowCount ? model.rowCount() : 0
}

function modelItem(model, index) {
    return model && model.get && index >= 0 && index < modelCount(model) ? model.get(index) : ({})
}

function modelIndexForItemId(model, itemId, fallbackIndex) {
    const count = modelCount(model)
    const id = String(itemId || "")
    if (id.length > 0) {
        for (let i = 0; i < count; ++i) {
            if (itemIdFor(model.get(i)) === id)
                return i
        }
        // A route's identity survives its source model being refreshed or
        // reused by another page. Never turn it into a different item.
        return -1
    }
    return count > 0 ? Math.max(0, Math.min(Number(fallbackIndex || 0), count - 1)) : -1
}

function detailsContext(args, fallbackModel) {
    const routeArgs = args || ({})
    const model = routeArgs.model || fallbackModel
    const index = modelIndexForItemId(model, routeArgs.itemId, routeArgs.focusIndex)
    const id = String(routeArgs.itemId || "")
    let item = modelItem(model, index)
    if (itemIdFor(item).length <= 0 && id.length > 0) {
        // The row this was opened from can be refilled by the page itself --
        // a series' seasons become the season's episodes. Fall back to the
        // copy taken at navigation, not a bare title, so nothing on the page
        // disappears while the full details load.
        const snapshot = routeArgs.snapshot
        item = snapshot && itemIdFor(snapshot) === id ? snapshot : {
                                                            movieId: id,
                                                            itemType: String(routeArgs.itemType || "Video"),
                                                            title: String(routeArgs.title || "Selected item"),
                                                            seriesId: String(routeArgs.seriesId || ""),
                                                            seasonId: String(routeArgs.seasonId || ""),
                                                            playable: true
                                                        }
    }
    if (!item.seriesName && String(routeArgs.seriesName || "").length > 0)
        item = Object.assign({}, item, {
                                 seriesName: String(routeArgs.seriesName)
                             })
    return {
        model: model,
        index: index,
        item: item,
        source: String(routeArgs.source || "movies"),
        returnRoute: String(routeArgs.returnRoute || "libraryGrid")
    }
}

function normalizeDetailsRoute(request, fallbackModel, currentRoute) {
    const nextModel = request && request.model ? request.model : fallbackModel
    const focusIndex = Math.max(0, Number(request && request.focusIndex !== undefined ? request.focusIndex : 0))
    const fallbackItem = modelItem(nextModel, focusIndex)
    const requestedItemId = request ? String(request.itemId || "") : ""
    const itemId = requestedItemId.length > 0 ? requestedItemId : itemIdFor(fallbackItem)
    if (itemId.length <= 0)
        return null

    return {
        model: nextModel,
        itemId: itemId,
        itemType: request && request.itemType ? String(request.itemType) : itemTypeFor(fallbackItem),
        source: request && request.source ? String(request.source) : "movies",
        returnRoute: request && request.returnRoute ? String(request.returnRoute) : (currentRoute || "libraryGrid"),
        focusIndex: focusIndex,
        title: request && request.title ? String(request.title) : String(fallbackItem.title || fallbackItem.seriesName
                                                                         || ""),
        seriesId: request && request.seriesId ? String(request.seriesId) : String(fallbackItem.seriesId || ""),
        seriesName: request && request.seriesName ? String(request.seriesName) : String(fallbackItem.seriesName || ""),
        seasonId: request && request.seasonId ? String(request.seasonId) : String(fallbackItem.seasonId || ""),
        snapshot: itemIdFor(fallbackItem) === itemId ? fallbackItem : null
    }
}

// Whether opening a details page from wherever we are should go on the stack
// or take the current frame's place.
//
// Details to details is a step further into the library -- an episode to its
// series, an item to something similar -- so it is pushed and Back comes back
// to where it was opened from. Landing back on details after playback is not a
// step: it stands in for the page playback was started from, and pushing it
// would put a page nobody navigated to between Back and the library. Reopening
// the same item is not a step either.
function detailsNavigationMode(currentRoute, currentArgs, nextArgs, source) {
    if (String(currentRoute || "") !== "itemDetails")
        return "push"
    if (String(source || "") === "playback")
        return "replace"
    const currentId = String((currentArgs || ({})).itemId || "")
    const nextId = String((nextArgs || ({})).itemId || "")
    return currentId.length > 0 && currentId === nextId ? "replace" : "push"
}

// `parent` names what the item belongs to when its own row may not say, as a
// series page does for its seasons.
function detailsRouteAt(model, index, source, returnRoute, currentRoute, parent) {
    const focusIndex = Math.max(0, Number(index || 0))
    const item = modelItem(model, focusIndex)
    return normalizeDetailsRoute({
                                     model: model,
                                     itemId: itemIdFor(item),
                                     itemType: itemTypeFor(item),
                                     title: String(item.title || item.seriesName || ""),
                                     seriesId: String(item.seriesId || (parent && parent.seriesId) || ""),
                                     seriesName: String(item.seriesName || (parent && parent.seriesName) || ""),
                                     seasonId: String(item.seasonId || ""),
                                     source: source || "movies",
                                     returnRoute: returnRoute || currentRoute || "libraryGrid",
                                     focusIndex: focusIndex
                                 }, model, currentRoute)
}
