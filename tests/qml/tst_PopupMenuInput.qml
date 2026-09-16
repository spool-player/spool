import QtQuick
import QtTest
import "../../qml/primitives" as Primitives

TestCase {
    id: testCase
    name: "PopupMenuInput"
    visible: true
    when: windowShown
    width: 640
    height: 480

    property int underlayClicks: 0
    property int rowActivations: 0
    property int buttonClicks: 0
    property int remoteActivations: 0

    Item {
        anchors.fill: parent

        TapHandler {
            onTapped: testCase.underlayClicks++
        }
    }

    Primitives.PopupMenuPanel {
        id: panel
        x: 120
        y: 80
        width: 360
        open: true
        openHeight: 240

        Item {
            id: button
            x: 10
            y: 10
            width: 132
            height: 44

            TapHandler {
                onTapped: testCase.buttonClicks++
            }
        }

        Item {
            id: remoteControl
            x: 180
            y: 10
            width: 132
            height: 44
            focus: true

            Keys.onReturnPressed: event => {
                testCase.remoteActivations++
                event.accepted = true
            }
            Keys.onEnterPressed: event => {
                testCase.remoteActivations++
                event.accepted = true
            }
            Keys.onSelectPressed: event => {
                testCase.remoteActivations++
                event.accepted = true
            }
        }

        Item {
            id: row
            x: 10
            y: 70
            width: parent.width - 20
            height: 48

            TapHandler {
                gesturePolicy: TapHandler.DragThreshold
                onTapped: testCase.rowActivations++
            }
        }

        Flickable {
            id: menuFlickable
            x: 10
            y: 128
            width: parent.width - 20
            height: 100
            contentHeight: 400
            boundsBehavior: Flickable.StopAtBounds
        }
    }

    function init() {
        underlayClicks = 0
        rowActivations = 0
        buttonClicks = 0
        remoteActivations = 0
        menuFlickable.cancelFlick()
        menuFlickable.contentY = 0
    }

    function touchTap(item, x, y) {
        const touch = touchEvent(item)
        touch.press(0, item, x, y).commit()
        wait(20)
        touch.release(0, item, x, y).commit()
    }

    function dragWithMouse(item, x, fromY, toY) {
        mousePress(item, x, fromY, Qt.LeftButton)
        const step = (toY - fromY) / 6
        for (let index = 1; index <= 6; ++index) {
            wait(16)
            mouseMove(item, x, fromY + step * index, 0, Qt.LeftButton)
        }
        mouseRelease(item, x, toY, Qt.LeftButton)
    }

    function dragWithTouch(item, x, fromY, toY) {
        const touch = touchEvent(item)
        touch.press(0, item, x, fromY).commit()
        const step = (toY - fromY) / 6
        for (let index = 1; index <= 6; ++index) {
            wait(16)
            touch.move(0, item, x, fromY + step * index).commit()
        }
        touch.release(0, item, x, toY).commit()
    }

    function test_rowClickDoesNotReachUnderlay() {
        const point = row.mapToItem(testCase, row.width / 2, row.height / 2)
        mouseClick(testCase, point.x, point.y, Qt.LeftButton)
        compare(rowActivations, 1)
        compare(underlayClicks, 0)
    }

    function test_buttonClickDoesNotReachUnderlay() {
        const point = button.mapToItem(testCase, button.width / 2, button.height / 2)
        mouseClick(testCase, point.x, point.y, Qt.LeftButton)
        compare(buttonClicks, 1)
        compare(underlayClicks, 0)
    }

    function test_remoteAcceptStillReachesFocusedContent() {
        remoteControl.forceActiveFocus()
        verify(remoteControl.activeFocus)
        keyClick(Qt.Key_Return)
        compare(remoteActivations, 1)
        keyClick(Qt.Key_Select)
        compare(remoteActivations, 2)
        compare(underlayClicks, 0)
    }

    function test_touchTapActivatesRowWithoutReachingUnderlay() {
        touchTap(row, row.width / 2, row.height / 2)
        compare(rowActivations, 1)
        compare(underlayClicks, 0)
    }

    function test_mouseDragStillScrollsMenu() {
        const x = menuFlickable.width / 2
        dragWithMouse(menuFlickable, x, 80, 10)
        tryVerify(function () {
            return menuFlickable.contentY > 20
        })
        compare(underlayClicks, 0)
    }

    function test_touchDragStillScrollsMenu() {
        const x = menuFlickable.width / 2
        dragWithTouch(menuFlickable, x, 80, 10)
        tryVerify(function () {
            return menuFlickable.contentY > 20
        })
        compare(underlayClicks, 0)
    }

    function test_emptyPanelSpaceDoesNotReachUnderlay() {
        const localX = panel.width - 5
        const localY = panel.height - 5
        verify(!button.contains(button.mapFromItem(panel, localX, localY)))
        verify(!remoteControl.contains(remoteControl.mapFromItem(panel, localX, localY)))
        verify(!row.contains(row.mapFromItem(panel, localX, localY)))
        verify(!menuFlickable.contains(menuFlickable.mapFromItem(panel, localX, localY)))
        const point = panel.mapToItem(testCase, localX, localY)
        mouseClick(testCase, point.x, point.y, Qt.LeftButton)
        compare(underlayClicks, 0)
    }

    function test_outsidePanelStillReachesUnderlay() {
        verify(!panel.contains(panel.mapFromItem(testCase, 20, 20)))
        mouseClick(testCase, 20, 20, Qt.LeftButton)
        compare(underlayClicks, 1)
    }
}
