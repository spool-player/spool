import QtQuick
import QtQuick.Templates as T
import QtTest
import "../../qml/shell" as Shell
import "../../qml/theme"

TestCase {
    id: testCase
    name: "SettingSyncControl"
    width: 800
    height: 500
    visible: true
    when: windowShown

    Component {
        id: controlScene
        FocusScope {
            id: scene
            width: 760
            height: 420
            property alias sync: syncAction
            property alias before: valueButton
            property alias after: followingButton
            property int toggleCount: 0
            property int valueCount: 0

            T.Button {
                id: valueButton
                x: 20
                y: 20
                width: 240
                height: 70
                focusPolicy: Qt.StrongFocus
                onClicked: scene.valueCount++
            }
            Shell.SettingSyncControl {
                id: syncAction
                x: 560
                y: 20
                settingKey: "audio/language"
                settingTitle: "Audio language"
                sourceLabel: "Jellyfin — Alice"
                syncState: ({
                                eligible: true,
                                enabled: true,
                                backend: "native",
                                status: "pending"
                            })
                actionFocused: false
                onToggled: scene.toggleCount++
            }
            T.Button {
                id: followingButton
                x: 20
                y: 150
                width: 240
                height: 70
                focusPolicy: Qt.StrongFocus
            }
        }
    }

    function state(status, backend, enabled) {
        return {
            eligible: true,
            enabled: enabled,
            backend: backend,
            status: status
        }
    }

    function test_onlyConfirmedSyncUsesFilledSuccessDot() {
        const scene = createTemporaryObject(controlScene, testCase)
        verify(scene)
        const control = scene.sync
        const dot = findChild(control, "syncStatusDot")
        verify(dot)
        for (const status of ["pending", "saving", "offline", "error", "unsupported"]) {
            control.syncState = state(status, "native", true)
            verify(!control.confirmedSynced)
            verify(control.statusColor !== Theme.success)
            compare(dot.color.a, 0)
        }
        control.syncState = state("synced", "native", true)
        verify(control.confirmedSynced)
        compare(control.statusColor, Theme.success)
        compare(dot.color, Theme.success)
        control.syncState = state("off", "native", false)
        verify(!control.confirmedSynced)
        verify(control.localOnly)
        compare(dot.color.a, 0)
        compare(control.Accessible.checked, false)
    }

    function test_channelAndSourceHelpChangesWithoutStealingFocus() {
        const scene = createTemporaryObject(controlScene, testCase)
        verify(scene)
        scene.before.forceActiveFocus()
        scene.sync.actionFocused = true
        const help = findChild(scene.sync, "syncHelp")
        verify(help)
        tryCompare(help, "visible", true)
        compare(help.contentItem.font.pixelSize, Metrics.bodySizePx)
        compare(scene.sync.iconName, "sync")
        verify(scene.sync.helpText.indexOf("service's own preference") >= 0)
        verify(scene.sync.helpText.indexOf("Alice") >= 0)
        verify(scene.before.activeFocus)
        scene.sync.sourceLabel = "Emby — Bob"
        scene.sync.syncState = state("pending", "spool", true)
        compare(scene.sync.iconName, "cloud_sync")
        verify(scene.sync.helpText.indexOf("Spool-specific sync") >= 0)
        verify(scene.sync.helpText.indexOf("Bob") >= 0)
        verify(scene.before.activeFocus)
        compare(scene.sync.Accessible.checked, true)
        compare(scene.sync.Accessible.description, scene.sync.helpText)
    }

    function test_hoverAndKeyboardHelpBothOpenAndClose() {
        const scene = createTemporaryObject(controlScene, testCase)
        verify(scene)
        const help = findChild(scene.sync, "syncHelp")
        mouseMove(testCase, 2, 2)
        tryCompare(help, "visible", false)
        mouseMove(scene.sync, scene.sync.width / 2, scene.sync.height / 2)
        tryCompare(help, "visible", true)
        mouseMove(testCase, 2, 2)
        tryCompare(help, "visible", false)
        scene.sync.actionFocused = true
        tryCompare(help, "visible", true)
        scene.sync.actionFocused = false
        tryCompare(help, "visible", false)
    }

    function test_syncClickIsIsolatedAndNotATabStop() {
        const scene = createTemporaryObject(controlScene, testCase)
        verify(scene)
        scene.before.forceActiveFocus()
        mouseClick(scene.sync, scene.sync.width / 2, scene.sync.height / 2)
        compare(scene.toggleCount, 1)
        compare(scene.valueCount, 0)
        verify(scene.before.activeFocus)
        verify(!scene.sync.activeFocus)
        keyClick(Qt.Key_Tab)
        tryCompare(scene.after, "activeFocus", true)
        scene.sync.syncState = {
            eligible: false,
            enabled: false,
            backend: "none",
            status: "unsupported"
        }
        verify(!scene.sync.visible)
        verify(scene.after.activeFocus)
    }

    function test_offlineAndErrorAreDistinguishableWithoutColor() {
        const scene = createTemporaryObject(controlScene, testCase)
        verify(scene)
        scene.sync.syncState = state("offline", "spool", true)
        const offline = scene.sync.statusLabel
        scene.sync.syncState = state("error", "spool", true)
        verify(scene.sync.statusLabel !== offline)
        scene.sync.syncState = state("off", "spool", false)
        verify(scene.sync.statusLabel !== offline)
        verify(scene.sync.localOnly)
    }
}
