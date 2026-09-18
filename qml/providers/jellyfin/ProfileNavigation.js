.pragma library

// Where a directional press lands in the account grid.
//
// The add-account tile is simply the cell after the last profile, so the grid
// is one uniform run of `profileCount + 1` cells and nothing has to carry a
// special case for it. Left and right walk that run; up and down step by a
// row. Returns -1 when the press has walked off the top or the left of the
// grid and belongs to whatever the page keeps above it.
function move(index, profileCount, columns, direction) {
    const total = Math.max(1, Math.round(profileCount) + 1)
    const columnCount = Math.max(1, Math.round(columns))
    const current = Math.max(0, Math.min(total - 1, Math.round(index)))

    switch (direction) {
    case "left":
        return current > 0 ? current - 1 : -1
    case "right":
        return Math.min(total - 1, current + 1)
    case "up":
        return current >= columnCount ? current - columnCount : -1
    case "down":
        // The last row is ragged, so stepping down from above it lands on the
        // add tile rather than refusing to move because that column is short.
        return current + columnCount < total ? current + columnCount : total - 1
    }
    return current
}
