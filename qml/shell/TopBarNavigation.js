.pragma library

// The rail's index space: the navigation buttons first, then whichever of the
// optional Remote, Cast and SyncPlay buttons the provider and the settings put
// up, in that order. A button that is not there takes no index, so Left and
// Right never land on something invisible.
function slots(railCount, remoteVisible, castVisible, syncVisible) {
    const result = []
    for (let index = 0; index < railCount; ++index)
        result.push({
                        "kind": "rail",
                        "railIndex": index
                    })
    if (remoteVisible)
        result.push({
                        "kind": "remote"
                    })
    if (castVisible)
        result.push({
                        "kind": "cast"
                    })
    if (syncVisible)
        result.push({
                        "kind": "sync"
                    })
    return result
}

function lastIndex(railCount, remoteVisible, castVisible, syncVisible) {
    return Math.max(0, slots(railCount, remoteVisible, castVisible, syncVisible).length - 1)
}

// The slot an index resolves to, clamped into the rail; null only when the
// rail is empty.
function slotAt(index, railCount, remoteVisible, castVisible, syncVisible) {
    const all = slots(railCount, remoteVisible, castVisible, syncVisible)
    if (all.length === 0)
        return null
    return all[Math.max(0, Math.min(all.length - 1, index))]
}

// Where a kind of button sits, or -1 when it is not on the rail.
function indexOf(kind, railCount, remoteVisible, castVisible, syncVisible) {
    const all = slots(railCount, remoteVisible, castVisible, syncVisible)
    for (let index = 0; index < all.length; ++index) {
        if (all[index].kind === kind)
            return index
    }
    return -1
}
