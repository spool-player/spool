import QtQuick
import "../theme"
import "../primitives"

FocusScope {
    id: router

    property var activeTarget: null
    property bool textInputActive: false
    property bool backspaceNavigatesInTextInput: false
    property bool webOsScanCodes: false
    property bool tvOsRemote: false
    property var backHandler: null
    property var globalHandler: null
    property int longPressInterval: 520

    property var armedTarget: null
    property int armedKey: 0
    property bool longPressHandled: false
    property bool pressActivated: false
    property bool backClaimed: false
    property int typeAheadKey: 0
    property int pressedDirectionKey: 0

    // Auto-repeat is a courtesy, not a guarantee, and every hold in the app is
    // built on it: a row that scrolls, a seek that runs, a slider that steps
    // all wait for the platform to say "still held" and stop the moment it
    // stops saying so. webOS says it in a dialect of its own -- synthetic
    // release/press pairs, sometimes without the flag, which is what
    // pressedDirectionKey below is for. Android does not say it at all, so
    // holding a direction there did nothing anywhere in the app.
    //
    // Rather than teach every view a third dialect, this speaks for the
    // platform where the platform is silent: a direction still down after the
    // delay repeats on a cadence until its release arrives. Views see exactly
    // the auto-repeat they already understand and none of them changes.
    //
    // The first hold of a session waits long enough that any platform which
    // repeats at all has already spoken -- desktop key repeat starts around
    // half a second on most systems -- so a slow deliberate press is never
    // mistaken for a hold on a platform that was going to handle it. Once one
    // hold has run with nothing heard, the wait drops to Android's own 400ms,
    // which is what the cadence below is too.
    property int sustainedHoldProbeDelay: 700
    property int sustainedHoldDelay: 400
    property int sustainedHoldInterval: 50
    // Nothing is held forever. A release that never arrives -- webOS has form
    // here -- would otherwise leave the app scrolling on its own.
    property int sustainedHoldLimit: 20000

    property bool platformSendsRepeats: false
    property bool platformSilent: false
    property int sustainedKey: 0
    property int sustainedModifiers: 0
    property double sustainedStartedAt: 0
    property bool sustainedRepeating: false

    // A third dialect, and the one an Android television box speaks. Holding a
    // direction on the Chromecast's remote produces neither a held key nor an
    // auto-repeat: it produces whole press/release pairs, about 70ms down and
    // 150-250ms apart, for as long as the key is held. Read literally that is
    // a person tapping six times a second, and every view in the app reads it
    // literally -- which is why a hold there steps, stutters and stops.
    //
    // A release that is followed by a press of the same key this soon was
    // never a release. Once one has been seen the dialect is known, and from
    // then on releases are held back for that long: if the press arrives the
    // key was still down and the press is the repeat the platform declined to
    // send, and if it does not the release was real and goes through this much
    // later. That lateness is the entire cost, it is paid only on platforms
    // that have already been caught lying, and the very first hold of a
    // session is what catches them -- from its second press onwards it is
    // already a hold.
    //
    // Only a television box is asked to prove it: a release that is really a
    // release, mistaken for a hold, is a far worse trade on a desktop or a
    // handset than the hold it would buy, and neither has ever spoken this
    // dialect. The remote still has to demonstrate it before anything is held
    // back, so a set-top box driven by a well-behaved remote is untouched too.
    property bool platformMayPairHolds: false
    property int releaseGrace: 300
    property bool platformPairsHolds: false
    property int lastReleaseKey: 0
    property double lastReleaseAt: 0
    property int heldReleaseKey: 0
    property int heldReleaseModifiers: 0

    focus: true
    onActiveTargetChanged: {
        flushHeldRelease()
        pressedDirectionKey = 0
    }
    // A window that loses focus never sends the release for whatever was down
    // when it went away.
    onActiveFocusChanged: if (!activeFocus) {
                              flushHeldRelease()
                              stopSustaining()
                          }
    Keys.priority: Keys.BeforeItem

    function stopSustaining() {
        sustainedTimer.stop()
        sustainedKey = 0
        sustainedRepeating = false
    }

    function beginSustaining(key, modifiers) {
        if (platformSendsRepeats)
            return
        sustainedKey = key
        sustainedModifiers = modifiers
        sustainedStartedAt = Date.now()
        sustainedRepeating = false
        sustainedTimer.restart()
    }

    // The stand-in, driven by a platform that does say when a key is down.
    // The cadence is the same; only what starts and stops it differs.
    function sustainPairedHold(key, modifiers) {
        if (sustainedKey !== key) {
            sustainedKey = key
            sustainedStartedAt = Date.now()
        }
        sustainedModifiers = modifiers
        sustainedRepeating = true
        sustainedTimer.restart()
    }

    function emitSustainedRepeat() {
        if (!sustainedKey)
            return
        if (Date.now() - sustainedStartedAt > sustainedHoldLimit) {
            stopSustaining()
            return
        }
        sustainedRepeating = true
        if (!platformSilent && !platformPairsHolds)
            console.info("input: no key repeats from this platform; holding a direction is driven here")
        if (!platformPairsHolds)
            platformSilent = true
        deliverDirection(sustainedKey, true, sustainedModifiers)
    }

    // Every release is remembered, whether or not it is held back, because the
    // first one cannot be: until a press has followed one this closely there
    // is nothing to say the platform is doing it on purpose.
    function noteRelease(key) {
        lastReleaseKey = key
        lastReleaseAt = Date.now()
    }

    // True when this press is the far side of a release that was not real.
    function pressContinuesHold(key) {
        if (heldReleaseKey === key) {
            heldReleaseTimer.stop()
            heldReleaseKey = 0
            return true
        }
        // A different key is genuinely down now, so whatever was waiting is up.
        if (heldReleaseKey)
            flushHeldRelease()
        if (!InputKeys.isDirection(key) || !platformMayPairHolds || platformPairsHolds || key !== lastReleaseKey || Date.now(
                    ) - lastReleaseAt >= releaseGrace)
            return false
        console.info("input: this platform holds a key by repeating press and release")
        platformPairsHolds = true
        return true
    }

    function flushHeldRelease() {
        if (!heldReleaseKey)
            return
        const key = heldReleaseKey
        const modifiers = heldReleaseModifiers
        heldReleaseKey = 0
        heldReleaseTimer.stop()
        // The event is long gone by now; a release only ever needed its
        // modifiers, and no release carries text.
        dispatchNormalized({
                               "modifiers": modifiers,
                               "text": ""
                           }, key, "release", false)
    }

    function backspaceNavigates() {
        return backspaceNavigatesInTextInput || !textInputActive
    }

    function normalizedKey(event) {
        // Qt's UIKit plugin reports Siri Remote Back/Menu as Menu. Normalize
        // both phases so a handled Back cannot leak its release to UIKit.
        // An unhandled root Back remains unaccepted for the system launcher.
        if (tvOsRemote && event.key === Qt.Key_Menu)
            return Qt.Key_Back
        if (webOsScanCodes && event.key === 0) {
            // LG's Wayland stack can lose the Qt key on physical remote
            // releases while retaining the XKB scan code. Recover directions
            // so release reaches the active view instead of waiting for its
            // repeat watchdog.
            const scanCode = Number(event.nativeScanCode || 0)
            if (scanCode === 111)
                return Qt.Key_Up
            if (scanCode === 113)
                return Qt.Key_Left
            if (scanCode === 114)
                return Qt.Key_Right
            if (scanCode === 116)
                return Qt.Key_Down
            if (scanCode === 406)
                return Qt.Key_Red
            if (scanCode === 407)
                return Qt.Key_Green
            if (scanCode === 408)
                return Qt.Key_Yellow
            if (scanCode === 409)
                return Qt.Key_Blue
        }
        if (InputKeys.isIgnoredPlayerNoise(event))
            return 0
        return InputKeys.isBackEvent(event, backspaceNavigates()) ? Qt.Key_Back : event.key
    }

    function routeDirection(key, phase, repeat, modifiers) {
        if (phase === "release") {
            // Qt-generated auto-repeat releases are not physical releases.
            if (!repeat && pressedDirectionKey === key) {
                pressedDirectionKey = 0
                stopSustaining()
            }
            if (activeTarget && activeTarget.directionRelease)
                router.deliver(activeTarget, key, phase, repeat)
            return true
        }

        // Some LG webOS remotes report repeated directional presses without
        // setting QKeyEvent::isAutoRepeat. A second press for a key that is
        // still physically down is a repeat; real rapid clicks have a release
        // between presses and therefore remain independent.
        const effectiveRepeat = repeat || pressedDirectionKey === key
        pressedDirectionKey = key
        if (effectiveRepeat && platformPairsHolds) {
            // Knowing the key is down is not the same as saying so often
            // enough. This remote re-presses every 150 to 270ms, and a view
            // watchdog that gives a hold 220ms to speak again ends it on the
            // slow ones -- which is a hold that accelerates for a while and
            // then stops for no reason the user can see. The platform says
            // whether the key is down; between its presses, this says it.
            sustainPairedHold(key, modifiers)
        } else if (effectiveRepeat) {
            // The platform repeats, in whichever dialect. Stand down for good.
            if (!platformSendsRepeats)
                console.info("input: the platform sends its own key repeats")
            platformSendsRepeats = true
            stopSustaining()
        } else {
            beginSustaining(key, modifiers)
        }
        return deliverDirection(key, effectiveRepeat, modifiers)
    }

    // The delivery half of a directional press, without the bookkeeping, so a
    // repeat this file made for itself takes exactly the path a real one does.
    function deliverDirection(key, repeat, modifiers) {
        const handled = router.deliver(activeTarget, key, "press", repeat)
        return handled || Boolean(globalHandler && globalHandler(key, "press", repeat, modifiers))
    }

    function deliver(target, key, phase, repeat) {
        return Boolean(target && target.routeKey && target.routeKey(key, phase, repeat))
    }

    function clearAccept() {
        longPressTimer.stop()
        armedTarget = null
        armedKey = 0
        longPressHandled = false
        pressActivated = false
    }

    function pressAccept(key, repeat) {
        if (repeat || armedTarget)
            return true
        const target = activeTarget
        if (!target || (!target.activate && !target.longPress))
            return router.deliver(target, key, "press", false)
        armedTarget = target
        armedKey = key
        longPressHandled = false
        pressActivated = false
        if (target.longPress) {
            longPressTimer.restart()
        } else {
            // Nothing to disambiguate without a long-press gesture: fire on
            // press so activation doesn't wait out the physical key release.
            pressActivated = true
            target.activate()
        }
        return true
    }

    function releaseAccept(key, repeat) {
        if (repeat)
            return true
        const target = armedTarget
        if (!target)
            return router.deliver(activeTarget, key, "release", false)
        longPressTimer.stop()
        const swallowed = longPressHandled
        const activatedOnPress = pressActivated
        clearAccept()
        if (swallowed && activeTarget && activeTarget.finishOpeningGesture)
            activeTarget.finishOpeningGesture()
        else if (!swallowed && !activatedOnPress && target.activate)
            target.activate()
        return true
    }

    function routeBack(phase, repeat) {
        if (phase === "release" && backClaimed)
            return true
        const target = activeTarget
        let claimed = router.deliver(target, Qt.Key_Back, phase, repeat)
        if (!claimed && target && target.back)
            claimed = Boolean(target.back())
        if (!claimed && backHandler)
            claimed = Boolean(backHandler())
        if (phase !== "release")
            backClaimed = claimed
        return claimed
    }

    function routeTypeAhead(event, key, phase) {
        if (textInputActive)
            return false
        if (phase === "release" && typeAheadKey === key) {
            typeAheadKey = 0
            return true
        }
        if (phase !== "press" || !event.text || event.text.length <= 0 || event.modifiers & (Qt.ControlModifier | Qt.AltModifier
                                                                                             | Qt.MetaModifier))
            return false
        if (!activeTarget || !activeTarget.typeAhead || !activeTarget.typeAhead(event.text))
            return false
        typeAheadKey = key
        return true
    }

    function dispatchNormalized(event, key, phase, repeat) {
        if (globalHandler && globalHandler(key, phase, repeat, event.modifiers))
            return true
        if (InputKeys.isBack(key, false)) {
            const handled = routeBack(phase, repeat)
            if (phase === "release")
                backClaimed = false
            return handled
        }
        if (routeTypeAhead(event, key, phase))
            return true
        // A focused field keeps the horizontal keys, which move its caret, but
        // never the vertical ones: a one-line field has no use for them and
        // swallowing them is what strands people in a form with no way out
        // but Escape. Everything else belongs to the field while it is being
        // typed into.
        if (textInputActive && !InputKeys.isVertical(key))
            return false
        if (InputKeys.isDirection(key))
            return routeDirection(key, phase, repeat, event.modifiers)
        if (textInputActive)
            return false
        if (InputKeys.isAccept(key))
            return phase === "press" ? pressAccept(key, repeat) : releaseAccept(key, repeat)
        if (router.deliver(activeTarget, key, phase, repeat))
            return true
        // What no one here has a use for is not nothing: a page may forward it
        // somewhere with bindings of its own. Text entry keeps its keys.
        return Boolean(activeTarget && activeTarget.unhandledKey && activeTarget.unhandledKey(key, phase, repeat,
                                                                                              event.modifiers,
                                                                                              event.text))
    }

    function dispatch(event, phase) {
        const key = normalizedKey(event)
        const repeat = Boolean(event.isAutoRepeat)
        if (key === 0)
            return true
        if (phase === "release") {
            noteRelease(key)
            // Qt's own auto-repeat releases are already understood downstream
            // and are never the dialect this holds back.
            if (platformPairsHolds && InputKeys.isDirection(key) && !repeat) {
                heldReleaseKey = key
                heldReleaseModifiers = event.modifiers
                heldReleaseTimer.restart()
                return true
            }
            return dispatchNormalized(event, key, phase, repeat)
        }
        return dispatchNormalized(event, key, phase, repeat || pressContinuesHold(key))
    }

    Keys.onPressed: event => {
        // A key press means the pointer is no longer what is being used, which
        // is the only reliable signal that a pointer which vanished without a
        // leave event is gone.
        Metrics.pointerActive = false
        event.accepted = dispatch(event, "press")
    }
    Keys.onReleased: event => event.accepted = dispatch(event, "release")

    Timer {
        id: sustainedTimer
        // Changing this restarts the timer, which is exactly what is wanted:
        // the wait to be sure of a hold, then the cadence of one.
        interval: router.sustainedRepeating ? router.sustainedHoldInterval : (router.platformSilent
                                                                              ? router.sustainedHoldDelay :
                                                                                router.sustainedHoldProbeDelay)
        repeat: true
        onTriggered: router.emitSustainedRepeat()
    }

    Timer {
        id: heldReleaseTimer
        interval: router.releaseGrace
        repeat: false
        onTriggered: router.flushHeldRelease()
    }

    Timer {
        id: longPressTimer
        interval: router.longPressInterval
        repeat: false
        onTriggered: {
            const target = router.armedTarget
            router.longPressHandled = Boolean(target && target.longPress && target.longPress())
        }
    }
}
