import QtQuick
import QtTest
import "../../qml/pages" as Pages

TestCase {
    id: testCase
    name: "PlayerOverlayInput"

    property var seekDeltas: []
    property var previewDeltas: []
    property int previewCommits: 0
    property int previewCancels: 0
    property double fakeNow: 1000
    property int subtitleCycles: 0
    property int controlsShown: 0
    property int fullscreenToggles: 0
    property int playbackToggles: 0

    QtObject {
        id: playerStub
        property string activeSegmentType: ""
        property double durationSeconds: 2400
        function cycleSubtitles() {
            ++testCase.subtitleCycles
        }
    }

    QtObject {
        id: overlayStub
        property bool hasPlayer: true
        property bool controlsVisible: true
        property string focusZone: "timeline"
        property int actionIndex: 1
        property var actions: ["back", "pause", "forward"]
        property var player: playerStub

        function canPreviewSeek() {
            return true
        }
        function seekBy(delta) {
            testCase.seekDeltas.push(delta)
        }
        function seekPreviewBy(delta) {
            testCase.previewDeltas.push(delta)
        }
        function commitSeekPreview() {
            ++testCase.previewCommits
        }
        function cancelSeekPreview() {
            ++testCase.previewCancels
        }
        function showControls(zone) {
            focusZone = zone;
            ++testCase.controlsShown
        }
        function toggleFullScreen() {
            ++testCase.fullscreenToggles
        }
        function togglePlayback() {
            ++testCase.playbackToggles
        }
        function maybeRestartAutohide() {
        }
    }

    Pages.PlayerOverlayInput {
        id: input
        overlay: overlayStub
    }

    function advance(milliseconds) {
        const end = fakeNow + milliseconds
        while (fakeNow < end) {
            fakeNow = Math.min(end, fakeNow + 16)
            input.seekHold.tick()
        }
    }

    function previewTotal() {
        let total = 0
        for (let index = 0; index < previewDeltas.length; ++index)
            total += previewDeltas[index]
        return total
    }

    function init() {
        seekDeltas = []
        previewDeltas = []
        previewCommits = 0
        previewCancels = 0
        fakeNow = 1000
        input.seekHold.tickInterval = 3600000
        input.seekHold.nowProvider = function () {
            return testCase.fakeNow
        }
        subtitleCycles = 0
        controlsShown = 0
        fullscreenToggles = 0
        playbackToggles = 0
        input.reset()
        overlayStub.controlsVisible = true
        overlayStub.focusZone = "timeline"
        overlayStub.actionIndex = 1
    }

    function test_shortSeekPreviewsTenSecondsAndCommitsOnRelease() {
        verify(input.pressed(Qt.Key_Left, false))
        compare(previewDeltas, [-10])
        verify(input.previewing)
        verify(input.released(Qt.Key_Left, false))
        compare(previewCommits, 1)
        verify(!input.previewing)
    }

    function test_actionRowNavigatesWithoutSeeking() {
        overlayStub.focusZone = "actions"
        verify(input.pressed(Qt.Key_Right, false))
        compare(seekDeltas, [])
        verify(input.released(Qt.Key_Right, false))
        compare(overlayStub.actionIndex, 2)
    }

    function test_leftFromFirstActionFocusesBackButton() {
        overlayStub.focusZone = "actions"
        overlayStub.actionIndex = 0
        verify(input.pressed(Qt.Key_Left, false))
        verify(input.released(Qt.Key_Left, false))
        compare(overlayStub.focusZone, "back")
        compare(overlayStub.actionIndex, 0)
    }

    function test_heldSeekRampsWithoutTouchingThePlayer() {
        verify(input.pressed(Qt.Key_Right, false))
        compare(previewDeltas, [10])

        // The nudge stands alone for a moment before the ramp picks it up.
        advance(200)
        compare(previewDeltas.length, 1)

        verify(input.pressed(Qt.Key_Right, true))
        advance(500)
        const cruised = previewTotal()
        verify(cruised > 10)
        verify(cruised < 45)

        advance(2500)
        const early = previewTotal() - cruised
        // The first seconds of the ramp stay steerable rather than bolting.
        verify(early > 100)
        verify(early < 600)

        advance(2500)
        const settled = previewTotal()
        advance(1000)
        // Past five seconds the rate is the running time's, near enough a third
        // of the file every second.
        verify(previewTotal() - settled > 700)
        compare(seekDeltas, [])
        compare(previewCommits, 0)

        verify(input.released(Qt.Key_Right, false))
        compare(previewCommits, 1)
    }

    function test_syntheticReleaseKeepsTheHoldAlive() {
        verify(input.pressed(Qt.Key_Right, false))
        verify(input.pressed(Qt.Key_Right, true))
        verify(input.released(Qt.Key_Right, true))
        compare(previewCommits, 0)
        verify(input.previewing)

        advance(1000)
        verify(previewTotal() > 10)
        verify(input.released(Qt.Key_Right, false))
        compare(previewCommits, 1)
    }

    function test_reversingDirectionKeepsThePreviewGoing() {
        verify(input.pressed(Qt.Key_Right, false))
        verify(input.pressed(Qt.Key_Right, true))
        advance(1000)
        verify(input.pressed(Qt.Key_Left, false))
        compare(previewCommits, 0)
        compare(previewDeltas[previewDeltas.length - 1], -10)
        verify(input.released(Qt.Key_Left, false))
        compare(previewCommits, 1)
    }

    function test_downRepeatCyclesAtBoundedRate() {
        for (let repeat = 0; repeat < 4; ++repeat)
            verify(input.pressed(Qt.Key_Down, true))
        compare(subtitleCycles, 2)
        compare(controlsShown, 2)
        verify(input.released(Qt.Key_Down, false))
        compare(input.downRepeats, 0)
    }

    function test_reservedKeyPressCannotReachMpv() {
        verify(input.pressed(Qt.Key_F, false))
        compare(fullscreenToggles, 0)
        verify(input.released(Qt.Key_F, false))
        compare(fullscreenToggles, 1)
    }

    function test_siriRemotePlayPauseTogglesOnceOnRelease() {
        overlayStub.controlsVisible = false
        verify(input.pressed(Qt.Key_MediaTogglePlayPause, false))
        compare(playbackToggles, 0)
        verify(input.released(Qt.Key_MediaTogglePlayPause, true))
        compare(playbackToggles, 0)
        verify(input.released(Qt.Key_MediaTogglePlayPause, false))
        compare(playbackToggles, 1)
        compare(controlsShown, 1)
        compare(overlayStub.focusZone, "actions")
    }

    function test_unusedKeyFallsThroughWithoutRevealingControls() {
        overlayStub.controlsVisible = false
        verify(!input.pressed(Qt.Key_J, false))
        verify(!input.released(Qt.Key_J, false))
        compare(controlsShown, 0)
    }

    function test_fTogglesFullscreen() {
        verify(input.released(Qt.Key_F, false))
        compare(fullscreenToggles, 1)
    }

    function test_modifierDoesNotRevealControls() {
        overlayStub.controlsVisible = false
        verify(!input.released(Qt.Key_Meta, false))
        compare(controlsShown, 0)
    }

    function test_verticalNavigationRevealsControls() {
        overlayStub.controlsVisible = false
        verify(input.released(Qt.Key_Up, false))
        compare(controlsShown, 1)
    }
}
