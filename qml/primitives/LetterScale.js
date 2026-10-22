.pragma library

// A name-sorted list's scroll bar is also its index. Each letter owns a
// stretch of the track as long as its share of the list, but never shorter
// than `minimum` (a fraction of the track), so a letter of three titles can
// still be pointed at. The thumb follows the same mapping, so it always sits
// in the stretch of the letter on screen.
//
// `starts` are where each letter begins, as fractions of the scroll range,
// in list order. The result maps scroll fractions to track fractions and back.
function scale(starts, minimum) {
    const count = starts.length
    if (count < 2)
        return null
    const from = []
    let previous = 0
    for (let i = 0; i < count; ++i) {
        previous = i === 0 ? 0 : Math.max(previous, Math.min(1, Number(starts[i]) || 0))
        from.push(previous)
    }
    from.push(1)
    const band = Math.max(0, Math.min(Number(minimum) || 0, 1 / count))
    const spare = 1 - band * count
    const to = [0]
    for (let i = 0; i < count; ++i)
        to.push(to[i] + band + spare * (from[i + 1] - from[i]))
    to[count] = 1
    return {
        "from": from,
        "to": to
    }
}

function clamp(value) {
    return Math.max(0, Math.min(1, value))
}

// The segment of `edges` holding `value`: the last that starts at or before it.
function segment(edges, value) {
    let index = 0
    while (index + 2 < edges.length && edges[index + 1] <= value)
        ++index
    return index
}

function toTrack(fraction, map) {
    const value = clamp(fraction)
    if (!map || value >= 1)
        return value
    const i = segment(map.from, value)
    const width = map.from[i + 1] - map.from[i]
    return width > 0 ? map.to[i] + (map.to[i + 1] - map.to[i]) * (value - map.from[i]) / width : map.to[i]
}

function fromTrack(fraction, map) {
    const value = clamp(fraction)
    if (!map || value >= 1)
        return value
    const i = segment(map.to, value)
    const width = map.to[i + 1] - map.to[i]
    return width > 0 ? map.from[i] + (map.from[i + 1] - map.from[i]) * (value - map.to[i]) / width : map.from[i]
}

// The letter whose stretch of the track holds `fraction`.
function indexAt(fraction, map) {
    return map ? segment(map.to, clamp(fraction)) : -1
}
