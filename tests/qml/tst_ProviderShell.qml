import QtQuick
import QtTest
import Spool
import "../../qml/shell" as Shell
import "../../qml/primitives" as Primitives

TestCase {
    id: testCase
    name: "ProviderShell"
    width: 1280
    height: 720
    visible: true
    when: windowShown
    Component {
        id: buttonComponent
        Primitives.ActionButton {
            kind: "primary"
            text: "Connect"
            width: 160
            height: 48
        }
    }
    Component {
        id: barComponent
        Shell.TopBar {
            width: testCase.width
            height: 64
        }
    }
    function luminance(color) {
        function linear(value) {
            return value <= 0.04045 ? value / 12.92 : Math.pow((value + 0.055) / 1.055, 2.4)
        }
        return 0.2126 * linear(color.r) + 0.7152 * linear(color.g) + 0.0722 * linear(color.b)
    }
    function test_buttonContrastAndDisabledInput() {
        const button = createTemporaryObject(buttonComponent, testCase)
        const spy = createTemporaryQmlObject('import QtTest; SignalSpy {}', testCase)
        spy.target = button
        spy.signalName = "clicked"
        for (const enabled of [false, true]) {
            button.enabled = enabled
            const a = luminance(button.foreground)
            const b = luminance(button.background.color)
            verify((Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05) >= 4.5, "Button label is readable")
            mouseClick(button, 80, 24)
            compare(spy.count, enabled ? 1 : 0)
        }
    }
    function test_deviceDropdownStaysOnPage() {
        const bar = createTemporaryObject(barComponent, testCase)
        const route = bar.currentRoute
        bar.openRemoteMenu()
        tryVerify(() => bar.remoteMenuOpen)
        compare(bar.currentRoute, route)
        const list = findChild(bar, "remoteControlsList")
        verify(list)
        verify(list.height > 100, "Device list has usable height: " + list.height)
        verify(list.width > 200)
        verify(list.count >= 2)
        bar.edge = "bottom"
        wait(20)
        verify(list.height > 100)
        bar.edge = "top"
        wait(20)
        verify(list.height > 100)
        compare(bar.lastIndex(), 3 + (bar.remoteVisible ? 1 : 0) + (bar.groupVisible ? 1 : 0))
        verify(bar.back())
        compare(bar.remoteMenuOpen, false)
        compare(bar.currentRoute, route)
    }
}
