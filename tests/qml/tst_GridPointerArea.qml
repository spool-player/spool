import QtQuick
import QtTest
import "../../qml/primitives" as Primitives

TestCase {
    id: testCase
    name: "GridPointerArea"
    visible: true
    when: windowShown
    width: 400
    height: 400

    Item {
        id: keyboardTarget
    }
    GridView {
        id: grid
        anchors.fill: parent
        model: 400
        cellWidth: 100
        cellHeight: 150
        flickableDirection: Flickable.VerticalFlick
        boundsBehavior: Flickable.StopAtBounds
        delegate: Rectangle {
            width: 100
            height: 150
            color: "gray"
        }
        Primitives.GridPointerArea {
            id: taps
            view: grid
        }
    }
    SignalSpy {
        id: activation
        target: taps
        signalName: "activated"
    }
    SignalSpy {
        id: context
        target: taps
        signalName: "contextRequested"
    }
    SignalSpy {
        id: contextEnded
        target: taps
        signalName: "contextGestureEnded"
    }

    function init() {
        grid.cancelFlick()
        grid.contentY = 0
        grid.currentIndex = 0
        grid.forceLayout()
        keyboardTarget.forceActiveFocus()
        activation.clear()
        context.clear()
        contextEnded.clear()
    }

    function tapAt(x, y) {
        const touch = touchEvent(grid)
        touch.press(0, grid, x, y).commit()
        wait(20)
        touch.release(0, grid, x, y).commit()
    }

    function test_tapOpensWithoutSelectingOrFocusingGrid() {
        tapAt(150, 75)
        compare(activation.count, 1)
        compare(activation.signalArguments[0][0], 1)
        compare(grid.currentIndex, 0)
        compare(keyboardTarget.activeFocus, true)
    }

    function test_swipeAndCatchDoNotActivateOrSelect() {
        const touch = touchEvent(grid)
        touch.press(0, grid, 150, 350).commit()
        for (let y = 320; y >= 80; y -= 30) {
            wait(16)
            touch.move(0, grid, 150, y).commit()
        }
        touch.release(0, grid, 150, 80).commit()
        verify(grid.contentY > 100)
        compare(activation.count, 0)
        compare(grid.currentIndex, 0)
        compare(keyboardTarget.activeFocus, true)
        grid.flick(0, -2000)
        wait(30)
        tapAt(150, 150)
        compare(activation.count, 0, "touching a coasting grid stops it, not opens a poster")
        compare(grid.currentIndex, 0)
        compare(keyboardTarget.activeFocus, true)
    }

    function test_scrolledTapUsesViewportCoordinates() {
        grid.contentY = 600
        grid.forceLayout()
        wait(30)
        tapAt(150, 75)
        compare(activation.count, 1)
        compare(activation.signalArguments[0][0], 17)
    }

    function test_holdOpensContextWithoutActivating() {
        const touch = touchEvent(grid)
        touch.press(0, grid, 150, 75).commit()
        tryCompare(context, "count", 1, 1200)
        touch.release(0, grid, 150, 75).commit()
        compare(context.signalArguments[0][0], 1)
        compare(contextEnded.count, 1)
        compare(activation.count, 0)
        compare(grid.currentIndex, 0)
    }
}
