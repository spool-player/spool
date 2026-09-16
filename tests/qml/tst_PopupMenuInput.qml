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
    }

    function init() {
        underlayClicks = 0
        rowActivations = 0
        buttonClicks = 0
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
}
