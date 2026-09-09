import QtQuick
import QtTest
import "../../qml/primitives" as Primitives

TestCase {
    id: testCase

    name: "InlineSlider"
    when: windowShown
    visible: true
    width: 400
    height: 320

    property int movedCount: 0
    property int committedCount: 0
    property real lastMoved: -1
    property real lastCommitted: -1
    // Stands in for the position a page feeds the slider from its own state.
    property real boundValue: 10

    Flickable {
        id: flick

        anchors.fill: parent
        contentWidth: width
        contentHeight: 800

        Primitives.InlineSlider {
            id: slider

            x: 40
            y: 140
            width: 320
            from: 0
            to: 100
            value: testCase.boundValue
            interactionMargin: 16
            onMoved: newValue => {
                testCase.movedCount++
                testCase.lastMoved = newValue
            }
            onCommitted: newValue => {
                testCase.committedCount++
                testCase.lastCommitted = newValue
            }
        }
    }

    function init() {
        flick.contentY = 0
        slider.interactionMargin = 16
        testCase.boundValue = 10
        testCase.movedCount = 0
        testCase.committedCount = 0
        testCase.lastMoved = -1
        testCase.lastCommitted = -1
    }

    function test_horizontalDragInsideFlickableMovesAndCommits() {
        const y = slider.y + slider.height / 2
        mousePress(flick, 72, y)
        mouseMove(flick, 180, y + 2)

        verify(slider.dragging, "the slider must keep the pointer grab from its Flickable")
        verify(slider.effectiveValue > 40, "the drawn value follows the horizontal drag")
        verify(testCase.lastMoved > 40, "the moved signal carries the dragged value")
        verify(testCase.movedCount > 0)
        compare(flick.contentY, 0, "horizontal scrubbing must not scroll the page")

        mouseMove(flick, 300, y + 2)
        verify(slider.effectiveValue > 75, "the slider keeps tracking after taking the grab")
        mouseRelease(flick, 300, y + 2)

        verify(!slider.dragging)
        compare(testCase.committedCount, 1, "release commits exactly one seek")
        verify(testCase.lastCommitted > 75, "the committed signal carries the released value")
    }

    // A drag used to assign `value` directly, which destroys the binding its
    // owner set. The slider then stopped following the thing it displays, so
    // playback progress arriving from elsewhere never moved the bar again.
    function test_dragKeepsTheOwnersValueBinding() {
        const y = slider.y + slider.height / 2
        mousePress(flick, 72, y)
        mouseMove(flick, 300, y + 2)
        mouseRelease(flick, 300, y + 2)

        compare(testCase.boundValue, 10, "the drag must not write back over the bound value")

        testCase.boundValue = 62
        compare(slider.value, 62, "the value binding must survive a drag")
        compare(slider.effectiveValue, 62, "the bar follows its owner again once the drag ends")
    }

    function test_repeatedPointsAtOneValueReportOnce() {
        const y = slider.y + slider.height / 2
        mousePress(flick, 200, y)
        const afterPress = testCase.movedCount
        mouseMove(flick, 200, y + 1)
        mouseMove(flick, 200, y + 2)
        compare(testCase.movedCount, afterPress, "a pointer that has not changed the value reports nothing")
        mouseRelease(flick, 200, y + 2)
    }

    function test_touchOutsideVisualTrackStillAdjustsSlider() {
        slider.interactionMargin = 0
        const touch = touchEvent(slider)
        const y = slider.height / 2 + 18
        verify(y > slider.height, "the touch is outside the drawn slider")
        touch.press(0, slider, 250, y).commit()
        touch.release(0, slider, 250, y).commit()
        compare(testCase.committedCount, 1)
        verify(testCase.lastCommitted > 70)
        compare(flick.contentY, 0)
    }
}
