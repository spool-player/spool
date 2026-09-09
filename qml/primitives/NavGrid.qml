import QtQuick
import "../theme"

GridView {
    id: root

    property var shell
    property int fallbackColumns: 1
    signal edgeUp
    signal accepted(int index)
    signal holdStarted(int key)
    readonly property bool directionRelease: true
    property int heldKey: 0
    property double holdStartedAt: 0
    property double lastHoldTickAt: 0
    property real holdAccumulator: 0
    property int holdReleaseTimeout: 220
    property int holdInitialReleaseTimeout: 800
    property bool holdRepeatSeen: false
    property int holdDelay: 500
    property int holdCruiseDuration: 700
    property int holdRampDuration: 1700
    property real holdTraversalSeconds: 5
    // Scales the rate axis only; hold delay, cruise and ramp timing stay put.
    property real holdSpeedMultiplier: 1
    readonly property real holdMaximumRate: Math.max(30, count / Math.max(1, holdTraversalSeconds))
                                            * holdSpeedMultiplier
    readonly property real holdInitialRate: Math.min(40 * holdSpeedMultiplier, holdMaximumRate)
    property int holdTickInterval: 16
    readonly property real focusRecoveryVisibleThreshold: 0.7
    property var nowProvider: function () {
        return Date.now()
    }

    focus: true
    keyNavigationEnabled: false
    highlightMoveDuration: 0
    // Share the touch coast and velocity limit with the home rows.
    flickDeceleration: Metrics.flickDecelerationPx
    maximumFlickVelocity: Metrics.maximumFlickVelocityPx
    flickableDirection: Flickable.VerticalFlick

    onActiveFocusChanged: if (!activeFocus)
                              stopAccelerating()
    onModelChanged: stopAccelerating()

    function columnCount() {
        return Math.max(1, Math.floor(width / Math.max(1, cellWidth || fallbackColumns)))
    }

    function topLeftVisibleIndex() {
        if (count <= 0 || !Number.isFinite(Number(cellHeight)) || cellHeight <= 0)
            return -1
        const columns = columnCount()
        const visibleTop = Math.max(0, Number(contentY) - Number(originY) - Number(topMargin || 0) - 0.5)
        const partialRow = Math.max(0, Math.floor(visibleTop / cellHeight))
        const visibleFraction = 1 - (visibleTop - partialRow * cellHeight) / cellHeight
        const row = visibleFraction >= focusRecoveryVisibleThreshold ? partialRow : partialRow + 1
        const candidate = row * columns
        if (candidate < count)
            return candidate
        return count - 1 - (count - 1) % columns
    }

    function moveBy(delta) {
        if (count <= 0)
            return true
        currentIndex = Math.max(0, Math.min(count - 1, currentIndex + delta))
        return true
    }

    function stopAccelerating() {
        holdTimer.stop()
        holdReleaseWatchdog.stop()
        heldKey = 0
        holdRepeatSeen = false
        holdAccumulator = 0
    }

    function nowMs() {
        return Number(nowProvider())
    }

    function beginAccelerating(key) {
        heldKey = key
        holdStartedAt = nowMs()
        lastHoldTickAt = holdStartedAt
        holdRepeatSeen = false
        holdAccumulator = 0
        holdTimer.start()
        holdReleaseWatchdog.restart()
    }

    function confirmHold() {
        if (!holdRepeatSeen) {
            holdRepeatSeen = true
            lastHoldTickAt = nowMs()
            holdStarted(heldKey)
        }
        holdReleaseWatchdog.restart()
    }

    function heldDelta() {
        const columns = columnCount()
        if (heldKey === Qt.Key_Left)
            return -1
        if (heldKey === Qt.Key_Right)
            return 1
        if (heldKey === Qt.Key_Up)
            return -columns
        return columns
    }

    function accelerationRate(heldMs) {
        if (heldMs < holdDelay)
            return 0
        const rampElapsed = heldMs - holdDelay - holdCruiseDuration
        if (rampElapsed <= 0)
            return holdInitialRate
        const progress = Math.min(1, rampElapsed / holdRampDuration)
        const eased = progress * progress * (3 - 2 * progress)
        return holdInitialRate + (holdMaximumRate - holdInitialRate) * eased
    }

    function accelerate() {
        if (!heldKey || !holdRepeatSeen || count <= 0)
            return
        const now = nowMs()
        const heldMs = Math.max(0, now - holdStartedAt)
        const frameMs = Math.min(50, Math.max(0, now - lastHoldTickAt))
        lastHoldTickAt = now
        const rate = accelerationRate(heldMs)
        if (rate <= 0)
            return
        holdAccumulator += rate * frameMs / 1000
        const moves = Math.floor(holdAccumulator)
        holdAccumulator -= moves
        if (moves <= 0)
            return

        // One index assignment per tick keeps accelerated traversal from
        // flooding the UI thread with stale selection work ahead of release.
        const requestedIndex = currentIndex + heldDelta() * moves
        moveBy(heldDelta() * moves)
        if (currentIndex !== requestedIndex)
            holdAccumulator = 0
    }

    function routeKey(key, phase, repeat) {
        if (phase === "release") {
            // Qt Wayland represents auto-repeat as a synthetic release/press
            // pair. Only the final physical release ends the hold.
            if (repeat)
                return true
            if (key === heldKey)
                stopAccelerating()
            return true
        }
        const directional = key === Qt.Key_Left || key === Qt.Key_Right || key === Qt.Key_Up || key === Qt.Key_Down
        if (!directional)
            return false
        if (key === heldKey) {
            // Only the platform's explicit auto-repeat flag can confirm a
            // hold. Unmarked presses are always independent one-step clicks,
            // regardless of their cadence.
            if (repeat) {
                if (!holdRepeatSeen)
                    moveBy(heldDelta())
                confirmHold()
            } else {
                beginAccelerating(key)
                moveBy(heldDelta())
            }
            return true
        }
        if (heldKey)
            stopAccelerating()
        if (repeat) {
            // Preserve acceleration if the first event observed by this view
            // is already marked as an auto-repeat.
            beginAccelerating(key)
            moveBy(heldDelta())
            confirmHold()
            return true
        }
        const columns = columnCount()
        const before = currentIndex
        if (key === Qt.Key_Left) {
            if (currentIndex % columns === 0)
                return true
            moveBy(-1)
        } else if (key === Qt.Key_Right) {
            moveBy(1)
        } else if (key === Qt.Key_Up) {
            if (currentIndex < columns) {
                edgeUp()
                return true
            }
            moveBy(-columns)
        } else {
            moveBy(columns)
        }
        if (currentIndex !== before)
            beginAccelerating(key)
        return true
    }

    function activate() {
        if (currentIndex >= 0)
            accepted(currentIndex)
    }

    Connections {
        target: root.model && typeof root.model === "object" ? root.model : null
        ignoreUnknownSignals: true

        function onModelReset() {
            root.stopAccelerating()
        }
    }

    Timer {
        id: holdTimer
        interval: root.holdTickInterval
        repeat: true
        onTriggered: root.accelerate()
    }

    Timer {
        id: holdReleaseWatchdog
        interval: root.holdRepeatSeen ? root.holdReleaseTimeout : root.holdInitialReleaseTimeout
        repeat: false
        onTriggered: root.stopAccelerating()
    }
}
