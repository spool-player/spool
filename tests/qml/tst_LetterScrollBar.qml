import QtQuick
import QtTest
import Spool
import "../../qml/primitives/LetterScale.js" as LetterScale

TestCase {
    id: testCase
    name: "LetterScrollBar"
    when: windowShown
    visible: true
    width: 400
    height: 400

    property var chosen: []

    GridView {
        id: grid
        anchors.fill: parent
        cellWidth: 100
        cellHeight: 100
        model: 400
        delegate: Rectangle {
            width: 90
            height: 90
            color: "#333333"
        }

        LetterScrollBar {
            id: bar
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            width: 30
            z: 4
            flickable: grid
            columns: 4
            rowHeight: grid.cellHeight
            onLetterChosen: index => testCase.chosen.push(index)
        }
    }

    // A over most of the list, then three letters crowded into its last rows.
    readonly property var letters: [
        {
            "label": "A",
            "row": 0
        },
        {
            "label": "B",
            "row": 380
        },
        {
            "label": "C",
            "row": 392
        },
        {
            "label": "D",
            "row": 396
        }
    ]

    function barX() {
        return grid.width - 10
    }

    function init() {
        bar.letters = letters
        grid.contentY = 0
        chosen = []
    }

    function test_scaleGivesEveryLetterRoomAndInverts() {
        const map = LetterScale.scale([0, 0.5, 0.98, 1, 1], 0.1)
        for (let i = 0; i + 1 < map.to.length; ++i)
            verify(map.to[i + 1] - map.to[i] >= 0.1 - 1e-9, "letter " + i + " is too short to point at")
        compare(LetterScale.toTrack(1, map), 1, "the end of the list is the end of the track")
        for (const fraction of [0, 0.1, 0.5, 0.75, 0.979, 0.99])
            fuzzyCompare(LetterScale.fromTrack(LetterScale.toTrack(fraction, map), map), fraction, 1e-9)
    }

    function test_tapOnTheTrackJumpsToTheLetterThere() {
        const map = bar.letterMap
        verify(map !== null)
        // The middle of C's stretch, where the pointer centres the thumb.
        const y = (map.to[2] + map.to[3]) / 2 * bar.availableTrack + bar.handleHeight / 2
        mouseClick(grid, barX(), y)
        compare(chosen, [2], "the crowded letter C was reachable")
        compare(grid.contentY, 0, "a tap leaves the move to the letter jump")
    }

    function test_dragScrubsAndTheThumbFollows() {
        const startY = bar.handleY + bar.handleHeight / 2
        mousePress(grid, barX(), startY)
        mouseMove(grid, barX(), startY + 100)
        mouseMove(grid, barX(), startY + 200)
        mouseRelease(grid, barX(), startY + 200)
        verify(grid.contentY > 0, "dragging the thumb scrolls")
        compare(chosen.length, 0, "a drag is not a tap")
        fuzzyCompare(bar.handleY + bar.handleHeight / 2, startY + 200, 2)
    }

    function test_withoutLettersTheTrackJumpsAtOnce() {
        bar.letters = []
        mousePress(grid, barX(), grid.height - 5)
        verify(grid.contentY > 0)
        mouseRelease(grid, barX(), grid.height - 5)
        compare(chosen.length, 0)
    }
}
