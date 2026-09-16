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
    property int underlayPresses: 0
    property int rowActivations: 0
    property int buttonClicks: 0
    property int remoteActivations: 0
    property int plainShieldButtonClicks: 0
    property int backdropClicks: 0

    Item {
        anchors.fill: parent

        TapHandler {
            onTapped: testCase.underlayClicks++
            onPressedChanged: if (pressed)
                                  testCase.underlayPresses++
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

    // OverlayDialog / RemoteControlMenu / SyncPlayMenu shape: a dismiss backdrop
    // MouseArea, then a plain surface carrying PopupShield directly.
    Item {
        id: dialog
        x: 20
        y: 340
        width: 400
        height: 120

        MouseArea {
            anchors.fill: parent
            onClicked: testCase.backdropClicks++
        }

        Item {
            id: plainSurface
            x: 100
            y: 10
            width: 200
            height: 100

            Primitives.PopupShield {}

            Item {
                id: plainShieldButton
                x: 10
                y: 10
                width: 120
                height: 40

                TapHandler {
                    onTapped: testCase.plainShieldButtonClicks++
                }
            }
        }
    }

    function init() {
        underlayClicks = 0
        underlayPresses = 0
        rowActivations = 0
        buttonClicks = 0
        remoteActivations = 0
        plainShieldButtonClicks = 0
        backdropClicks = 0
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
        compare(underlayPresses, 1)
    }

    function test_plainShieldButtonDoesNotReachUnderlayOrBackdrop() {
        const point = plainShieldButton.mapToItem(testCase, plainShieldButton.width / 2, plainShieldButton.height / 2)
        mouseClick(testCase, point.x, point.y, Qt.LeftButton)
        compare(plainShieldButtonClicks, 1)
        compare(backdropClicks, 0)
        compare(underlayClicks, 0)
        // Press-level too: a passive handler beneath must not even see pressed,
        // since PlayerOverlayChrome acts on press, not tap.
        compare(underlayPresses, 0)
    }

    function test_plainShieldEmptySpaceDoesNotReachUnderlayOrBackdrop() {
        const point = plainSurface.mapToItem(testCase, plainSurface.width - 5, plainSurface.height - 5)
        mouseClick(testCase, point.x, point.y, Qt.LeftButton)
        compare(plainShieldButtonClicks, 0)
        compare(backdropClicks, 0)
        compare(underlayClicks, 0)
    }

    function test_plainShieldBackdropStillDismisses() {
        const point = dialog.mapToItem(testCase, 20, dialog.height / 2)
        verify(!plainSurface.contains(plainSurface.mapFromItem(testCase, point.x, point.y)))
        mouseClick(testCase, point.x, point.y, Qt.LeftButton)
        compare(backdropClicks, 1)
        compare(plainShieldButtonClicks, 0)
    }
}
