import QtQuick
import QtTest
import "../../qml/primitives" as Primitives

TestCase {
    id: testCase
    name: "ListScrollBar"
    when: windowShown
    visible: true
    width: 400
    height: 400

    property int delegatePresses: 0
    ListModel {
        id: shiftedModel
    }

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

        // Stands in for LibraryGridPage's delegate MouseArea, which fills the
        // view at z 3 and used to swallow every press aimed at the scroll bar.
        MouseArea {
            anchors.fill: parent
            z: 3
            onPressed: testCase.delegatePresses++
        }

        Primitives.ListScrollBar {
            id: bar
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            width: 30
            z: 4
            flickable: grid
            minimumSize: 0.04
        }
    }

    function barX() {
        return grid.width - 15
    }

    function init() {
        grid.model = 400
        grid.contentY = 0
        testCase.delegatePresses = 0
    }

    function test_pressOnEmptyTrackJumpsAndDoesNotReachDelegates() {
        verify(bar.visible, "scroll bar is visible while the content overflows")
        verify(bar.handleCenterY < 40, "handle starts at the top")

        mousePress(grid, barX(), 300)
        compare(testCase.delegatePresses, 0, "the delegate MouseArea must not swallow the press")
        verify(bar.pressed, "the bar reports pressed while scrubbing")
        fuzzyCompare(bar.handleCenterY, 300, 6, "handle centres on the press point")
        const jumped = grid.contentY
        verify(jumped > 0, "content jumps to the press position")

        mouseMove(grid, barX(), 360)
        verify(grid.contentY > jumped, "the press continues as a drag")
        fuzzyCompare(bar.handleCenterY, 360, 6, "handle keeps tracking the cursor")

        mouseMove(grid, barX(), 120)
        verify(grid.contentY < jumped, "dragging back up scrubs backwards")

        mouseRelease(grid, barX(), 120)
        verify(!bar.pressed)
    }

    function test_pressOnHandleKeepsGrabOffset() {
        grid.contentY = grid.contentHeight / 2
        const before = grid.contentY
        const grabY = Math.round(bar.handleCenterY + 4)

        mousePress(grid, barX(), grabY)
        compare(grid.contentY, before, "pressing the handle itself must not lurch")

        mouseMove(grid, barX(), grabY + 20)
        verify(grid.contentY > before, "handle dragging still scrolls")
        mouseRelease(grid, barX(), grabY + 20)
    }

    function test_pressAtTrackEndsClampsToContentEnds() {
        mousePress(grid, barX(), 2)
        mouseRelease(grid, barX(), 2)
        compare(grid.contentY, 0, "the top of the track is the start of the content")

        mousePress(grid, barX(), grid.height - 2)
        mouseRelease(grid, barX(), grid.height - 2)
        fuzzyCompare(grid.contentY, grid.contentHeight - grid.height, 1, "the bottom of the track is the end")
    }

    function test_shiftedOriginTrackStillReachesBothEnds() {
        shiftedModel.clear()
        for (let index = 0; index < 400; ++index)
            shiftedModel.append({
                                    "value": index
                                })
        grid.model = shiftedModel
        grid.forceLayout()
        grid.contentY = 5000
        shiftedModel.remove(0, 160)
        grid.forceLayout()
        verify(grid.originY > 0)
        mousePress(grid, barX(), 2)
        mouseRelease(grid, barX(), 2)
        fuzzyCompare(grid.contentY, grid.originY, 1)
        mousePress(grid, barX(), grid.height - 2)
        mouseRelease(grid, barX(), grid.height - 2)
        fuzzyCompare(grid.contentY, grid.originY + grid.contentHeight - grid.height, 1)
    }

    function test_nonInteractiveBarIgnoresPresses() {
        bar.interactive = false
        mousePress(grid, barX(), 300)
        mouseRelease(grid, barX(), 300)
        compare(grid.contentY, 0, "a non-interactive bar does not scrub")
        compare(testCase.delegatePresses, 1, "the press falls through to the grid instead")
        bar.interactive = true
    }
}
