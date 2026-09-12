import QtQuick
import QtTest
import "../../qml/pages" as Pages

TestCase {
    id: testCase

    name: "PlayerSeekBarHover"
    when: windowShown
    visible: true
    width: 800
    height: 300

    QtObject {
        id: playerDouble

        property real durationSeconds: 120
        property bool buffering: false
        property real effectivePlaybackSpeed: 1
        property bool trickplayAvailable: true
        property string activeSegmentType: ""
        property real activeSegmentEndSeconds: 0
        property var chapters: []

        function trickplayForSeconds(seconds) {
            return {
                available: true,
                width: 160,
                height: 90,
                sheetWidth: 160,
                sheetHeight: 90,
                offsetX: 0,
                offsetY: 0,
                url: ""
            }
        }
    }

    QtObject {
        id: overlayDouble

        property var player: playerDouble
        property bool hasPlayer: true
        property bool audioOnly: false
        property real uiScale: 1
        property bool controlsVisible: true
        property string focusZone: "timeline"
        property bool scrubbing: false
        property bool remoteScrubbing: false
        property bool previewing: false
        property string controlsReason: "local"
        property real scrubSeconds: 0
        property bool timelineHovering: false
        property real timelineHoverSeconds: 0
        property real committedSeconds: -1

        function dp(value) {
            return value
        }
        function formatClock(seconds) {
            return String(seconds)
        }
        function positionSeconds() {
            return 30
        }
        function isControlsActive() {
            return controlsVisible
        }
        function clampSeconds(seconds) {
            return Math.max(0, Math.min(player.durationSeconds, seconds))
        }
        function showControls(zone) {
            controlsVisible = true
            focusZone = zone
            controlsReason = "local"
            remoteScrubbing = false
        }
        function commitScrub() {
            committedSeconds = scrubSeconds
            scrubbing = false
            return true
        }
    }

    Pages.PlayerSeekBar {
        id: seekBar

        anchors.centerIn: parent
        width: 600
        height: 100
        overlay: overlayDouble
    }

    Pages.PlayerTrickplayPreview {
        id: preview
        width: 800
        overlay: overlayDouble
    }

    function init() {
        mouseMove(testCase, 0, 0)
        overlayDouble.controlsReason = "local"
        overlayDouble.remoteScrubbing = false
        overlayDouble.timelineHovering = false
        overlayDouble.timelineHoverSeconds = 0
        overlayDouble.committedSeconds = -1
        overlayDouble.scrubbing = false
    }

    function test_hoverAboveAndBelowTrackPreviewsPosition() {
        mouseMove(seekBar, 150, 5)
        verify(overlayDouble.timelineHovering)
        // Hover previews a position without committing a seek.
        compare(overlayDouble.committedSeconds, -1)
        compare(Math.round(overlayDouble.timelineHoverSeconds), 30)

        mouseMove(seekBar, 450, 95)
        verify(overlayDouble.timelineHovering)
        compare(Math.round(overlayDouble.timelineHoverSeconds), 90)
        verify(!overlayDouble.scrubbing)
    }

    function test_hoverCannotMoveKeyboardScrub() {
        overlayDouble.scrubbing = true
        overlayDouble.scrubSeconds = 70
        mouseMove(seekBar, 150, 5)
        mouseMove(seekBar, 450, 95)
        compare(overlayDouble.scrubSeconds, 70)
        compare(preview.previewSeconds, 70)
        compare(overlayDouble.committedSeconds, -1)
    }

    function test_remotePreviewIgnoresParkedPointerAfterTeardown() {
        mouseMove(seekBar, 150, 5)
        overlayDouble.controlsReason = "remote"
        overlayDouble.remoteScrubbing = true
        overlayDouble.scrubSeconds = 80
        verify(preview.visible)
        compare(preview.previewSeconds, 80)

        overlayDouble.remoteScrubbing = false
        verify(!preview.visible, "remote teardown must not reveal the parked pointer's thumbnail")
        compare(overlayDouble.committedSeconds, -1)
    }

    function test_pressTakesOverRemotePreviewAndReleaseSeeks() {
        overlayDouble.controlsReason = "remote"
        overlayDouble.remoteScrubbing = true
        overlayDouble.scrubSeconds = 80
        mousePress(seekBar, 150, 50)
        verify(overlayDouble.scrubbing)
        mouseMove(seekBar, 450, 50)
        compare(Math.round(preview.previewSeconds), 90)
        mouseRelease(seekBar, 450, 50)
        compare(Math.round(overlayDouble.committedSeconds), 90)
        verify(!overlayDouble.scrubbing)
    }
}
