pragma Singleton

import QtQuick

QtObject {
    readonly property real focusRecoveryVisibleThreshold: 0.7
    function focus(item) {
        if (item && (Metrics.keyboardFocusActive || isTextInputItem(item)))
            item.forceActiveFocus()
    }

    function positionChild(view, item) {
        if (!view || !item)
            return
        Qt.callLater(function () {
            if (view && item)
                view.positionViewAtChild(item, Flickable.Contain)
        })
    }

    function topLeftVisibleCandidate(view, clipItem) {
        if (!view || !clipItem || !Number.isFinite(Number(view.count)) || view.count <= 0)
            return null
        const clipBounds = {
            "left": 0,
            "top": 0,
            "right": Number(clipItem.width),
            "bottom": Number(clipItem.height)
        }
        const viewport = view.mapToItem(clipItem, 0, 0, view.width, view.height)
        const viewportLeft = Math.max(clipBounds.left, viewport.x)
        const viewportTop = Math.max(clipBounds.top, viewport.y)
        const viewportRight = Math.min(clipBounds.right, viewport.x + viewport.width)
        const viewportBottom = Math.min(clipBounds.bottom, viewport.y + viewport.height)
        if (viewportRight <= viewportLeft || viewportBottom <= viewportTop)
            return null

        let best = null
        for (let index = 0; index < view.count; ++index) {
            const item = view.itemAtIndex(index)
            if (!item)
                continue
            const rect = item.mapToItem(clipItem, 0, 0, item.width, item.height)
            const left = Math.max(viewportLeft, rect.x)
            const top = Math.max(viewportTop, rect.y)
            const right = Math.min(viewportRight, rect.x + rect.width)
            const bottom = Math.min(viewportBottom, rect.y + rect.height)
            if (right <= left || bottom <= top)
                continue
            const itemArea = Math.max(1, rect.width * rect.height)
            const visibleFraction = (right - left) * (bottom - top) / itemArea
            const fullyVisible = rect.x >= viewportLeft - 0.5 && rect.y >= viewportTop - 0.5 && rect.x + rect.width
                  <= viewportRight + 0.5 && rect.y + rect.height <= viewportBottom + 0.5
            const candidate = {
                "index": index,
                "top": top,
                "left": left,
                "fullyVisible": fullyVisible,
                "visibleFraction": visibleFraction
            }
            if (!best || earlierVisibleCandidate(candidate, best))
                best = candidate
        }
        return best
    }

    function earlierVisibleCandidate(candidate, current) {
        if (!candidate)
            return false
        if (!current)
            return true
        const candidatePreferred = Boolean(candidate.fullyVisible) || Number(candidate.visibleFraction || 0)
              >= focusRecoveryVisibleThreshold

        const currentPreferred = Boolean(current.fullyVisible) || Number(current.visibleFraction || 0)
              >= focusRecoveryVisibleThreshold

        if (candidatePreferred !== currentPreferred)
            return candidatePreferred
        if (candidate.top !== current.top)
            return candidate.top < current.top
        if (candidate.left !== current.left)
            return candidate.left < current.left
        return candidate.index < current.index
    }

    function focusIndexWithoutScrolling(view, index) {
        if (!view || !Number.isFinite(Number(index)) || index < 0 || index >= view.count)
            return false
        const contentX = Number(view.contentX)
        const contentY = Number(view.contentY)
        view.currentIndex = index
        view.forceActiveFocus()
        if (Number.isFinite(contentX))
            view.contentX = contentX
        if (Number.isFinite(contentY))
            view.contentY = contentY
        return view.currentIndex === index
    }

    function isAccept(key, includeSpace) {
        return key === Qt.Key_Return || key === Qt.Key_Enter || key === Qt.Key_Select || (includeSpace !== false && key
                                                                                          === Qt.Key_Space)
    }

    function isBack(key, includeBackspace, includeBrowserBack) {
        return key === Qt.Key_Back || key === Qt.Key_Escape || (includeBackspace !== false && key === Qt.Key_Backspace)
                || (includeBrowserBack !== false && key === Qt.Key_BrowserBack)
    }

    function hasProperty(item, name) {
        return item && typeof item[name] !== "undefined"
    }

    function isTextInputItem(item) {
        return hasProperty(item, "cursorPosition") && hasProperty(item, "selectedText") && hasProperty(item, "text") && (hasProperty(
                                                                                                                             item, "echoMode")
                                                                                                                         || hasProperty(
                                                                                                                             item, "inputMethodHints")
                                                                                                                         || hasProperty(
                                                                                                                             item, "textFormat"))
    }

    function isBackEvent(event, includeBackspace, includeBrowserBack) {
        const scanCode = Number(event.nativeScanCode || 0)
        const virtualKey = Number(event.nativeVirtualKey || 0)
        const key = Number(event.key || 0)
        return isBack(event.key, includeBackspace === true, includeBrowserBack) || event.key === 0x01200003 || key
                === 461 || scanCode === 420 || scanCode === 461 || virtualKey === 420 || virtualKey === 461
    }

    function isDirection(key) {
        return isHorizontal(key) || isVertical(key)
    }

    function isHorizontal(key) {
        return key === Qt.Key_Left || key === Qt.Key_Right
    }

    function isVertical(key) {
        return key === Qt.Key_Up || key === Qt.Key_Down
    }

    function isMedia(key) {
        return key === Qt.Key_MediaPlay || key === Qt.Key_Play || key === Qt.Key_MediaTogglePlayPause
    }

    function isMediaNext(key) {
        return key === Qt.Key_MediaNext
    }

    function isMediaPrevious(key) {
        return key === Qt.Key_MediaPrevious
    }

    function isColor(key) {
        return key === Qt.Key_Red || key === Qt.Key_Green || key === Qt.Key_Yellow || key === Qt.Key_Blue
    }

    function isIgnoredPlayerNoise(event) {
        const scanCode = Number(event.nativeScanCode || 0)
        return event.key === 0 && (scanCode === 1206 || scanCode === 1207)
    }
}
