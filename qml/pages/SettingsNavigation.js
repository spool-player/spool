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

// One focus owner per row. Subfocus is logical, never another tab/vertical stop.
function normalizeSyncMode(mode, hasSync) {
    return mode === "sync-action" && !hasSync ? "row" : mode
}

function syncRoute(mode, action, hasSync, editsValue) {
    mode = normalizeSyncMode(mode, hasSync)
    if (action === "up" || action === "down")
        return { "mode": "row", "effect": action === "up" ? "move-up" : "move-down" }
    if (mode === "value-editing") {
        if (action === "activate" || action === "back")
            return { "mode": "row", "effect": "end-edit" }
        return { "mode": mode, "effect": action === "left" || action === "right" ? "value" : "none" }
    }
    if (mode === "sync-action") {
        if (action === "left" || action === "back")
            return { "mode": "row", "effect": "none" }
        return { "mode": mode, "effect": action === "activate" ? "toggle-sync" : "none" }
    }
    if (action === "right" && hasSync)
        return { "mode": "sync-action", "effect": "none" }
    if (action === "activate")
        return editsValue ? { "mode": "value-editing", "effect": "begin-edit" }
                          : { "mode": "row", "effect": "activate" }
    if (action === "back")
        return { "mode": "row", "effect": "back" }
    if (action === "left" && hasSync && editsValue)
        return { "mode": "row", "effect": "none" }
    return { "mode": "row", "effect": action === "left" || action === "right" ? "value" : "none" }
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

// Value editors retain their dependency/HDR rules. Sync controls deliberately
// ignore those rules, but never cross a platform or Never-policy boundary.
function extraSyncRows(schema, platform, reachableKeys) {
    const reachable = {}
    for (let index = 0; index < reachableKeys.length; ++index)
        reachable[reachableKeys[index]] = true
    const rows = []
    for (let index = 0; index < schema.length; ++index) {
        const spec = schema[index]
        if (!reachable[spec.key] && platformSupported(spec, platform)
                && (spec.syncPolicy === "portable" || spec.syncPolicy === "device"))
            rows.push(spec)
    }
    return rows
}
