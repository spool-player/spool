import QtQuick
import QtTest
import Spool

TestCase {
    id: testCase
    name: "OptionPickerInput"
    width: 1280
    height: 720
    visible: true
    when: windowShown
    property int underlayClicks: 0

    Item {
        id: stage
        anchors.fill: parent
    }

    MouseArea {
        parent: stage
        anchors.fill: parent
        onClicked: testCase.underlayClicks++
    }

    Rectangle {
        id: pickerAnchor
        parent: stage
        x: 200
        y: 120
        width: 360
        height: 60
    }

    OptionPickerDialog {
        id: picker
        parent: stage
        visible: true
        options: ["First", "Second", "Third"]
        currentIndex: 0
        anchorItem: pickerAnchor
    }

    SignalSpy {
        id: selectedSpy
        target: picker
        signalName: "selected"
    }

    function init() {
        selectedSpy.clear()
        underlayClicks = 0
        picker.currentIndex = 0
        pickerAnchor.width = 360
        verify(picker.completePresentation())
        verify(picker.placementReady)
    }

    function test_directionMovesOnPressOnly() {
        verify(picker.routeKey(Qt.Key_Down, "press", false))
        compare(picker.currentIndex, 0)
        compare(selectedSpy.count, 0)
        verify(picker.routeKey(Qt.Key_Down, "release", false))
        compare(picker.currentIndex, 0)
        compare(selectedSpy.count, 0)
        verify(picker.routeKey(Qt.Key_Select, "press", false))
        compare(selectedSpy.count, 0)
        verify(picker.routeKey(Qt.Key_Select, "release", false))
        compare(selectedSpy.count, 0)
        picker.activate()
        compare(selectedSpy.count, 1)
        compare(selectedSpy.signalArguments[0][0], 1)
    }

    function test_wideAnchorMakesLongOptionsReadable() {
        pickerAnchor.width = 680
        verify(picker.panelWidth >= 520)
    }

    function test_pointerSelectionDoesNotClickThrough() {
        const panel = findChild(picker, "optionPickerPanel")
        verify(panel)
        const point = panel.mapToItem(testCase, panel.width / 2, Metrics.scaled(8) + picker.rowHeight / 2)
        mouseClick(testCase, point.x, point.y, Qt.LeftButton)
        compare(selectedSpy.count, 1)
        compare(selectedSpy.signalArguments[0][0], 0)
        compare(underlayClicks, 0)
    }
}
