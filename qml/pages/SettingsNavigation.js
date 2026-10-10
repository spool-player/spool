.pragma library

// `platform` is the Platform singleton. It is passed whole rather than as a
// growing list of booleans, because every caller has it and each new
// form-factor question would otherwise add another positional argument.
function platformSupported(row, platform) {
    if (!row)
        return false
    if (row.platform === "desktop" && platform.isTV)
        return false
    if (row.platform === "webos" && !platform.isWebOS)
        return false
    if (row.platform === "android" && !platform.isAndroid)
        return false
    return true
}

function rowAvailable(row, platform, hdrPlayback, valueForKey) {
    if (!platformSupported(row, platform))
        return false
    if (row.requiresHdrPlayback && !hdrPlayback)
        return false
    if (row.dependsOnKey && String(valueForKey(row.dependsOnKey)) !== String(row.dependsOnValue))
        return false
    return true
}

// Flatten hand-authored sections into a MenuListView model. resolve(key)
// returns the spec to show for a key, or a falsy value to leave it out; a
// section whose rows all drop out takes its header with it. Header entries are
// tagged so MenuListView's default rowEnabled skips over them.
function sectionedRows(sections, resolve) {
    const rows = []
    for (let s = 0; s < sections.length; ++s) {
        const section = sections[s]
        const visible = []
        for (let k = 0; k < section.keys.length; ++k) {
            const spec = resolve(section.keys[k])
            if (spec) {
                visible.push({
                                 "section": false,
                                 "spec": spec
                             })
            }
        }
        if (visible.length === 0)
            continue
        rows.push({
                      "section": true,
                      "spec": {
                          "title": section.title
                      }
                  })
        for (let v = 0; v < visible.length; ++v)
            rows.push(visible[v])
    }
    return rows
}

function firstActionableRow(rows, start) {
    for (let index = Math.max(0, start); index < rows.length; ++index) {
        if (!rows[index].section)
            return index
    }
    return -1
}

function detailLevel(row) {
    return row && row.level !== undefined ? Number(row.level) : 0
}

function clampIndex(index, count) {
    if (count <= 0)
        return -1
    return Math.max(0, Math.min(count - 1, index))
}

function rowCount(rows) {
    if (!rows)
        return 0
    return rows.count !== undefined ? Number(rows.count) : Number(rows.length || 0)
}

function rowAt(rows, index) {
    if (!rows || index < 0 || index >= rowCount(rows))
        return null
    return rows.get ? rows.get(index) : rows[index]
}

function indexForRowKey(rows, key) {
    for (let index = 0; index < rowCount(rows); ++index) {
        const row = rowAt(rows, index)
        if (row && row.rowKey === key)
            return index
    }
    return -1
}

function nearestRowKey(rows, sourceIndex) {
    let best = null
    let bestDistance = Number.POSITIVE_INFINITY
    for (let index = 0; index < rowCount(rows); ++index) {
        const row = rowAt(rows, index)
        if (!row)
            continue
        const distance = Math.abs(Number(row.sourceIndex) - Number(sourceIndex))
        const precedes = Number(row.sourceIndex) <= Number(sourceIndex)
        const bestPrecedes = best && Number(best.sourceIndex) <= Number(sourceIndex)
        if (distance < bestDistance || (distance === bestDistance && precedes && !bestPrecedes)) {
            best = row
            bestDistance = distance
        }
    }
    return best ? String(best.rowKey || "") : ""
}

function reconcileRows(model, nextRows) {
    const oldCount = rowCount(model)
    const nextCount = rowCount(nextRows)
    let prefix = 0
    while (prefix < oldCount && prefix < nextCount
           && rowAt(model, prefix).rowKey === rowAt(nextRows, prefix).rowKey)
        ++prefix

    let suffix = 0
    while (suffix < oldCount - prefix && suffix < nextCount - prefix
           && rowAt(model, oldCount - suffix - 1).rowKey === rowAt(nextRows, nextCount - suffix - 1).rowKey)
        ++suffix

    const removeCount = oldCount - prefix - suffix
    if (removeCount > 0)
        model.remove(prefix, removeCount)
    const insertCount = nextCount - prefix - suffix
    for (let index = 0; index < insertCount; ++index)
        model.insert(prefix + index, rowAt(nextRows, prefix + index))
    if (model.set) {
        for (let index = 0; index < prefix; ++index)
            model.set(index, rowAt(nextRows, index))
        for (let offset = 0; offset < suffix; ++offset) {
            const index = nextCount - suffix + offset
            model.set(index, rowAt(nextRows, index))
        }
    }
}

function valueRoute(mode, action, editsValue) {
    if (action === "up" || action === "down")
        return { "mode": "row", "effect": action === "up" ? "move-up" : "move-down" }
    if (mode === "value-editing") {
        if (action === "activate" || action === "back")
            return { "mode": "row", "effect": "end-edit" }
        return { "mode": mode, "effect": action === "left" || action === "right" ? "value" : "none" }
    }
    if (action === "activate")
        return editsValue ? { "mode": "value-editing", "effect": "begin-edit" }
                          : { "mode": "row", "effect": "activate" }
    if (action === "back")
        return { "mode": "row", "effect": "back" }
    return { "mode": "row", "effect": action === "left" || action === "right" ? "value" : "none" }
}

var categories = [
    { "id": "appearance", "title": "Appearance" },
    { "id": "playback", "title": "Playback" },
    { "id": "subtitles", "title": "Subtitles" },
    { "id": "streaming", "title": "Streaming" },
    { "id": "sources", "title": "Sources" },
    { "id": "downloads", "title": "Downloads" },
    { "id": "diagnostics", "title": "Diagnostics & About" }
]

function categoryTitle(id) {
    for (let index = 0; index < categories.length; ++index)
        if (categories[index].id === id)
            return categories[index].title
    return "Settings"
}

function matchesSearch(row, query, choices) {
    const terms = String(query || "").toLocaleLowerCase().trim().split(/\s+/)
    const text = [row.title, row.description, row.key, row.searchKeywords, categoryTitle(row.categoryId)]
          .concat(row.choiceValues || [], choices || row.choiceLabels || []).join(" ").toLocaleLowerCase()
    for (let index = 0; index < terms.length; ++index)
        if (text.indexOf(terms[index]) < 0)
            return false
    return true
}

// Recover in the viewport that the pointer left behind, never scroll back to
// an old anchor. InputKeys owns candidate preference and visibility threshold.
function recoverVisibleSelection(view, clipItem, inputKeys) {
    const selected = view.itemAtIndex(view.currentIndex)
    const candidate = inputKeys.topLeftVisibleCandidate(view, clipItem)
    if (view.activeFocus && selected) {
        const rect = selected.mapToItem(clipItem, 0, 0, selected.width, selected.height)
        const viewport = view.mapToItem(clipItem, 0, 0, view.width, view.height)
        const width = Math.max(0, Math.min(rect.x + rect.width, viewport.x + viewport.width, clipItem.width)
                               - Math.max(rect.x, viewport.x, 0))
        const height = Math.max(0, Math.min(rect.y + rect.height, viewport.y + viewport.height, clipItem.height)
                                - Math.max(rect.y, viewport.y, 0))
        if (width * height / Math.max(1, rect.width * rect.height) >= inputKeys.focusRecoveryVisibleThreshold)
            return false
        // At large zoom a row may exceed the whole viewport. InputKeys' partial
        // candidate fallback still makes the visible part of that row usable.
        if (candidate && candidate.index === view.currentIndex && rect.height > view.height && height > 0)
            return false
    }
    if (candidate)
        inputKeys.focusIndexWithoutScrolling(view, candidate.index)
    return true
}

// A recovery press is not an edit gesture. Qt may send synthetic releases
// between repeats; only the physical release permits the next action.
function consumeRecoveryGesture(view, clipItem, inputKeys, gesture, key, phase, repeat) {
    if (phase === "release") {
        if (!repeat && gesture.key === key)
            gesture.key = 0
        return true
    }
    if (gesture.key === key)
        return true
    if (!recoverVisibleSelection(view, clipItem, inputKeys))
        return false
    gesture.key = key
    return true
}

var subtitleSections = [
    { "title": "Size and position", "keys": ["subtitles/scalePercent", "subtitles/verticalPositionPercent",
        "subtitles/alwaysOverridePositionAndSize", "subtitles/allowInBlackBars"] },
    { "title": "Colour", "keys": ["subtitles/overrideTextColor", "subtitles/textColor"] },
    { "title": "Which subtitles", "keys": ["subtitles/language", "subtitles/mode"] }
]
var subtitleAdvancedSections = [
    { "title": "Text style", "keys": ["subtitles/styling", "subtitles/textWeight", "subtitles/font",
        "subtitles/dropShadow", "subtitles/textBackground"] },
    { "title": "Image subtitles", "keys": ["subtitles/recolorImageSubtitles", "subtitles/bitmapSharpnessPercent",
        "subtitles/bitmapShadowEnabled"] },
    { "title": "Image subtitle shadow", "keys": ["subtitles/bitmapShadowCoreSize", "subtitles/bitmapShadowCoreGrow",
        "subtitles/bitmapShadowCoreOpacityPercent", "subtitles/bitmapShadowSpreadEnabled",
        "subtitles/bitmapShadowSpreadSize", "subtitles/bitmapShadowSpreadGrow", "subtitles/bitmapShadowSpreadX",
        "subtitles/bitmapShadowSpreadY", "subtitles/bitmapShadowSpreadOpacityPercent", "subtitles/bitmapShadowDither"] },
    { "title": "HDR", "keys": ["subtitles/hdrBrightnessPercent"] },
    { "title": "Start over", "keys": ["action/resetSubtitleAppearance"] }
]

function subtitleReachableKeys(schema, platform, hdrPlayback, valueForKey) {
    const keys = {}
    const sections = subtitleSections.concat(subtitleAdvancedSections)
    for (let s = 0; s < sections.length; ++s)
        for (let k = 0; k < sections[s].keys.length; ++k)
            keys[sections[s].keys[k]] = true
    const result = []
    for (let index = 0; index < schema.length; ++index) {
        const row = schema[index]
        if (keys[row.key] && rowAvailable(row, platform, hdrPlayback, valueForKey))
            result.push(row.key)
    }
    return result
}
