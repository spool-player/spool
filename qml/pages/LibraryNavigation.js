.pragma library

// Prefer a prefix anywhere in traversal order to an earlier substring match,
// but fetch/case-fold each native model item at most once per keystroke.
function typeAheadIndex(count, currentIndex, query, includeCurrent, titleAt) {
    if (count <= 0 || query.length <= 0)
        return -1
    const start = Math.max(0, currentIndex)
    const firstOffset = includeCurrent ? 0 : 1
    let partialMatch = -1
    for (let offset = firstOffset; offset < firstOffset + count; ++offset) {
        const index = (start + offset) % count
        const match = titleAt(index).indexOf(query)
        if (match === 0)
            return index
        if (match > 0 && partialMatch < 0)
            partialMatch = index
    }
    return partialMatch
}

function value(item, key) {
    if (!item)
        return undefined
    const result = item[key]
    return result === undefined || result === null ? undefined : result
}

function nameSection(item) {
    const title = String(value(item, "sortName") || value(item, "title") || value(item, "displayTitle") || value(item,
                                                                                                                 "seriesName")
                         || "").trim()
    if (title.length === 0)
        return "#"
    const initial = title.charAt(0)
    const upper = initial.toLocaleUpperCase()
    return upper !== initial.toLocaleLowerCase() ? upper : "#"
}

function yearSection(dateValue, fallbackYear) {
    const match = String(dateValue || "").match(/^(\d{4})/)
    if (match)
        return match[1]
    const year = Number(fallbackYear || 0)
    return year > 0 ? String(Math.round(year)) : "#"
}

function decimalSection(numberValue) {
    const number = Number(numberValue)
    return isFinite(number) ? number.toFixed(1) : "#"
}

function sectionLabel(sortBy, item) {
    switch (String(sortBy || "SortName")) {
    case "SortName":
        return nameSection(item)
    case "PremiereDate":
        return yearSection(value(item, "premiereDate"), value(item, "year"))
    case "DateCreated":
        return yearSection(value(item, "dateCreated"), value(item, "year"))
    case "DatePlayed":
    case "SeriesDatePlayed":
        return yearSection(value(item, "datePlayed"), value(item, "year"))
    case "DateLastContentAdded":
        return yearSection(value(item, "dateLastContentAdded"), value(item, "year"))
    case "CommunityRating":
        return decimalSection(value(item, "communityRating"))
    case "CriticRating":
        return decimalSection(value(item, "criticRating"))
    case "OfficialRating":
        return String(value(item, "officialRating") || "#").toLocaleUpperCase()
    case "PlayCount":
        return String(Math.max(0, Math.round(Number(value(item, "playCount") || 0))))
    case "Runtime":
    {
        const ticks = Number(value(item, "runtimeTicks") || 0)
        return ticks > 0 ? String(Math.max(1, Math.round(ticks / 600000000))) + " min" : "#"
    }
    default:
        return "#"
    }
}
