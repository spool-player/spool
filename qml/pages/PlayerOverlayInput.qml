import QtQuick
import "../primitives"

Item {
    id: root
    visible: false

    required property var overlay
    property int seekKey: 0
    property int downRepeats: 0
    readonly property bool previewing: seekKey !== 0
    property alias seekHold: seekHold

    // A tap nudges by tapSeconds. A held key picks up where the tap left off
    // and builds, over five seconds, to a rate set by the running time: about a
    // third of the file every second, so the far end is always within reach.
    readonly property real tapSeconds: 10
    readonly property real holdInitialRate: 50
    readonly property real holdMaximumRate: {
        const duration = overlay.hasPlayer ? Number(overlay.player.durationSeconds) || 0 : 0
        return Math.max(400, Math.min(3600, duration / 3))
    }

    function reset() {
        const wasSeeking = seekKey !== 0
        seekKey = 0
        downRepeats = 0
        seekHold.stopTracking()
        // A scrub the pointer owns is not ours to drop.
        if (wasSeeking)
            overlay.cancelSeekPreview()
    }

    function seekDelta(key) {
        return key === Qt.Key_Left ? -1 : key === Qt.Key_Right ? 1 : 0
    }

    // Every repeat moves the preview, never mpv: the seek lands once, when the
    // key comes back up.
    HoldNavigationController {
        id: seekHold

        stepDelay: 300
        initialRate: root.holdInitialRate
        maximumRate: root.holdMaximumRate
        cruiseDuration: 700
        // Five seconds from a standing start to full speed, held back early so
        // the first seconds stay steerable and the last ones cover ground.
        rampDuration: 4300
        rampShape: 2.2
        stepCallback: function (key, steps, source) {
            const seconds = source === "press" ? root.tapSeconds : steps
            root.overlay.seekPreviewBy(root.seekDelta(key) * seconds)
        }
        onHoldReleased: root.overlay.commitSeekPreview()
    }

    function pressed(key, repeat) {
        // These actions run on release, but their press belongs to Spool too.
        // Otherwise an unhandled press could also trigger the user's mpv binding.
        if (key === Qt.Key_F || key === Qt.Key_I || key === Qt.Key_Info || key === Qt.Key_Q || key === Qt.Key_S || key
                === Qt.Key_A || key === Qt.Key_T || InputKeys.isMedia(key) || InputKeys.isMediaPrevious(key)
                || InputKeys.isMediaNext(key) || InputKeys.isColor(key))
            return true
        const delta = seekDelta(key)
        if (delta !== 0 && overlay.controlsVisible && overlay.focusZone !== "timeline")
            return true
        if (delta !== 0 && overlay.hasPlayer && overlay.canPreviewSeek()) {
            seekKey = key
            return seekHold.routeKey(key, "press", repeat)
        }
        if (key === Qt.Key_Down && repeat && overlay.hasPlayer) {
            ++downRepeats
            if (downRepeats === 1 || downRepeats % 4 === 0) {
                overlay.player.cycleSubtitles()
                overlay.showControls("actions")
            }
            return true
        }
        if (!repeat && key !== Qt.Key_Down)
            downRepeats = 0
        return false
    }

    function handleControlsKey(key) {
        if (key === Qt.Key_Up) {
            overlay.focusZone = overlay.focusZone === "actions" ? "timeline" : "back"
            overlay.showControls(overlay.focusZone)
            return true
        }
        if (key === Qt.Key_Down) {
            if (overlay.focusZone === "actions" && overlay.actions[overlay.actionIndex] === "pause") {
                overlay.openAudioSync()
                return true
            }
            if (overlay.focusZone === "timeline")
                overlay.actionIndex = overlay.pauseActionIndex
            overlay.focusZone = overlay.focusZone === "back" ? "timeline" : "actions"
            overlay.showControls(overlay.focusZone)
            return true
        }
        if (key === Qt.Key_Left) {
            if (overlay.focusZone === "timeline") {
                overlay.seekBy(-10)
            } else if (overlay.focusZone === "actions") {
                const moved = overlay.actionIndex > 0
                if (moved)
                    --overlay.actionIndex
                else
                    overlay.focusZone = "back"
            }
            overlay.showControls(overlay.focusZone)
            return true
        }
        if (key === Qt.Key_Right) {
            if (overlay.focusZone === "timeline")
                overlay.seekBy(10)
            else if (overlay.focusZone === "actions")
                overlay.actionIndex = Math.min(overlay.actions.length - 1, overlay.actionIndex + 1)
            overlay.showControls(overlay.focusZone)
            return true
        }
        return false
    }

    function dispatchRemapAction(action) {
        if (!overlay.hasPlayer || !action || action === "none")
            return false
        switch (action) {
        case "togglePause":
            overlay.togglePlayback()
            overlay.showControls("actions")
            break
        case "toggleSubs":
            overlay.player.toggleSubtitles()
            break
        case "cycleSubs":
            overlay.player.cycleSubtitles()
            break
        case "cycleAudio":
            overlay.player.cycleAudio()
            break
        case "skipBack10":
            overlay.seekBy(-10)
            break
        case "skipForward10":
            overlay.seekBy(10)
            break
        case "skipBack30":
            overlay.seekBy(-30)
            break
        case "skipForward30":
            overlay.seekBy(30)
            break
        case "skipBack90":
            overlay.seekBy(-90)
            break
        case "skipForward90":
            overlay.seekBy(90)
            break
        case "skipBackAndEnableSubs":
            overlay.seekBy(-10)
            overlay.player.enableSubtitles()
            break
        case "skipSegment":
            overlay.player.skipActiveSegment()
            break
        case "queuePrevious":
            overlay.playPrevious()
            break
        case "queueNext":
            overlay.playNext()
            break
        case "showInfo":
            overlay.toggleDebugStats()
            break
        case "stop":
            overlay.player.stopWithReason("remap-stop")
            break
        default:
            return false
        }
        return true
    }

    function released(key, repeat) {
        if (key === seekKey) {
            // A synthetic release from key auto-repeat is not the end of the
            // gesture; the controller tells them apart. Either way the key is
            // spoken for until it physically comes up, so a stale release
            // cannot fall through and seek a second time.
            seekHold.routeKey(key, "release", repeat)
            if (!repeat) {
                seekKey = 0
                overlay.maybeRestartAutohide()
            }
            return true
        }
        if (key === Qt.Key_Down && downRepeats > 0) {
            downRepeats = 0
            return true
        }
        if (overlay.controlsVisible && handleControlsKey(key))
            return true
        if (key === Qt.Key_T && overlay.hasPlayer && overlay.player.activeSegmentType.length > 0) {
            overlay.player.skipActiveSegment()
            return true
        }
        if (key === Qt.Key_F && overlay.hasPlayer) {
            overlay.toggleFullScreen()
            return true
        }
        if (!overlay.controlsVisible && (key === Qt.Key_Up || key === Qt.Key_Down)) {
            overlay.showControls("timeline")
            return true
        }
        if (key === Qt.Key_Meta || key === Qt.Key_Control || key === Qt.Key_Alt || key === Qt.Key_Shift)
            return false
        if (InputKeys.isColor(key) && dispatchRemapAction(overlay.colorAction(key)))
            return true
        if ((key === Qt.Key_I || key === Qt.Key_Info) && overlay.hasPlayer) {
            overlay.toggleDebugStats()
            return true
        }
        if (key === Qt.Key_Q && overlay.hasPlayer) {
            overlay.player.stopWithReason("player-q")
            return true
        }
        if (InputKeys.isMediaPrevious(key)) {
            overlay.playPrevious()
            return true
        }
        if (InputKeys.isMediaNext(key)) {
            overlay.playNext()
            return true
        }
        if (InputKeys.isMedia(key) && overlay.hasPlayer) {
            if (!repeat) {
                overlay.togglePlayback()
                overlay.showControls("actions")
            }
            return true
        }
        if (key === Qt.Key_S) {
            overlay.openMenu("subtitles")
            return true
        }
        if (key === Qt.Key_A) {
            overlay.openMenu("audio")
            return true
        }
        return repeat && InputKeys.isDirection(key)
    }

    function routeKey(key, phase, repeat) {
        return phase === "press" ? pressed(key, repeat) : released(key, repeat)
    }
}
