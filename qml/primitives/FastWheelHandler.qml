import QtQuick
import "../theme"

WheelHandler {
    id: root

    required property var flickable
    property bool horizontal: false
    // Touchpad: pixelDelta is already in device pixels, so this is a gentle gain.
    property real touchpadMultiplier: 1.6
    // Mouse wheel: pixels travelled per notch (one notch == 120 angle units).
    // Kept deliberately modest so a single notch is a comfortable nudge rather
    // than a full-screen jump.
    property int stepPixels: 120
    property int animationDuration: 90
    signal scrolled

    target: null
    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad

    function clamp(value, minValue, maxValue) {
        return Math.max(minValue, Math.min(maxValue, value))
    }

    function scrollBase(propertyName) {
        if (root.scrollAnimation.running && root.scrollAnimation.target === flickable && root.scrollAnimation.property
                === propertyName)
            return root.scrollAnimation.to
        const current = Number(flickable ? flickable[propertyName] : 0)
        return Number.isFinite(current) ? current : 0
    }

    function stopScrolling() {
        root.scrollAnimation.stop()
    }

    function moveFlickable(propertyName, value, animate) {
        if (!flickable || flickable[propertyName] === undefined || !Number.isFinite(Number(value)))
            return
        const current = Number(flickable[propertyName])
        if (!Number.isFinite(current))
            return
        if (!animate || Theme.reducedMotion || animationDuration <= 0) {
            root.scrollAnimation.stop()
            flickable[propertyName] = value
            return
        }

        root.scrollAnimation.stop()
        root.scrollAnimation.target = flickable
        root.scrollAnimation.property = propertyName
        root.scrollAnimation.from = current
        root.scrollAnimation.to = value
        root.scrollAnimation.start()
    }

    onWheel: event => {
        if (!flickable)
            return
        const pixelDelta = root.horizontal ? (event.pixelDelta.x !== 0 ? event.pixelDelta.x : event.pixelDelta.y) : (
                                                 event.pixelDelta.y !== 0 ? event.pixelDelta.y : event.pixelDelta.x)
        const angleDelta = root.horizontal ? (event.angleDelta.x !== 0 ? event.angleDelta.x : event.angleDelta.y) : (
                                                 event.angleDelta.y !== 0 ? event.angleDelta.y : event.angleDelta.x);
        // High-resolution devices (touchpads) report pixelDelta and scroll
        // smoothly; classic mouse wheels report angleDelta in 120-unit notches
        // and get a fixed, modest step per notch.
        const delta = pixelDelta !== 0 ? pixelDelta * root.touchpadMultiplier : angleDelta / 120 * root.stepPixels

        if (delta === 0)
            return
        const animate = pixelDelta === 0
        if (root.horizontal) {
            const minX = Number(flickable.originX) || 0
            const maxX = minX + Math.max(0, flickable.contentWidth - flickable.width)
            root.moveFlickable("contentX", clamp(root.scrollBase("contentX") - delta, minX, maxX), animate)
        } else {
            const minY = Number(flickable.originY) || 0
            const maxY = minY + Math.max(0, flickable.contentHeight - flickable.height)
            root.moveFlickable("contentY", clamp(root.scrollBase("contentY") - delta, minY, maxY), animate)
        }
        root.scrolled()
        event.accepted = true
    }

    property NumberAnimation scrollAnimation: NumberAnimation {
        duration: root.animationDuration
        easing.type: Easing.OutCubic
        onFinished: root.scrolled()
    }
}
