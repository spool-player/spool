import QtQuick
import QtQuick.Window
import QtTest
import "../../qml/pages" as Pages
import "../../qml/shell" as Shell
import "../../qml/theme"

TestCase {
    id: testCase
    name: "ExtensionIntegration"
    width: 1280
    height: 720
    visible: true
    when: windowShown

    Rectangle {
        id: scene
        anchors.fill: parent
        color: Theme.bg
        Pages.SettingsPage {
            id: settingsPage
            anchors.fill: parent
        }
        Pages.SettingsSyncPage {
            id: syncPage
            anchors.fill: parent
            visible: false
        }
        Pages.SubtitleSettingsPanel {
            id: subtitlePage
            anchors.fill: parent
            visible: false
        }
        Pages.RemoteControlPage {
            id: remotePage
            anchors.fill: parent
            visible: false
        }
        Shell.ProviderSurface {
            id: surface
            anchors.fill: parent
            visible: context !== null
        }
    }
    Binding {
        target: Metrics
        property: "viewportWidth"
        value: testCase.width
    }
    Binding {
        target: Metrics
        property: "viewportHeight"
        value: testCase.height
    }
    Binding {
        target: Metrics
        property: "zoomPercent"
        value: Settings.uiScalePercent
    }

    function cycle() {
        const before = Integration.cycles
        SettingsSync.retry()
        tryVerify(() => Integration.cycles > before && !SettingsSync.busy, 10000)
    }
    function state(key) {
        return SettingsSync.states[key] || ({})
    }
    function selectSetting(key) {
        settingsPage.reconcileSettingsRows(settingsPage.rebuildVisibleRows(), key, true)
        compare(settingsPage.currentRow().key, key)
    }
    function record(identity, key) {
        return Integration.stats(identity).document.entries[key].value
    }
    function capture(name) {
        const path = Integration.capturePath(name)
        if (path.length) {
            waitForRendering(scene)
            const image = grabImage(scene)
            compare(image.width, testCase.width)
            compare(image.height, testCase.height)
            image.save(path)
        }
    }
    function test_endToEnd() {
        tryVerify(() => testCase.Window.window.active && Qt.application.state === Qt.ApplicationActive, 10000,
                  "Provider integration requires a natively active application window")
        tryVerify(() => Integration.running(Integration.first) && Integration.running(Integration.second), 10000)
        tryVerify(() => Integration.cycles > 0 && !SettingsSync.busy, 10000)
        compare(SettingsSync.accountId, Integration.first)
        compare(Settings.uiScalePercent, 100)
        tryCompare(settingsPage, "contentReady", true)

        // Real QML value editors -> Settings -> durable intent -> worker HTTP ->
        // stateful loopback -> read-back -> real per-setting status.
        settingsPage.setRowValue(settingsPage.rowsByKey["audio/trackMode"], "Smart", -1)
        selectSetting("theme/reducedMotion")
        settingsPage.activateRow(settingsPage.currentRow(), settingsPage.currentIndex)
        cycle()
        compare(Integration.stats("first").native.audioMode, "Smart")
        compare(state("theme/reducedMotion").status, "synced", JSON.stringify(state("theme/reducedMotion")))
        compare(record("first", "theme/reducedMotion"), true)
        compare(state("audio/trackMode").backend, "native")
        compare(state("theme/reducedMotion").backend, "spool")
        compare(state("audio/trackMode").status, "synced")
        compare(state("theme/reducedMotion").status, "synced")

        // Customization is isolated on the sync page, not a horizontal focus
        // stop beside every ordinary value editor.
        settingsPage.visible = false
        syncPage.visible = true
        syncPage.forceActiveFocus()
        syncPage.activateRow(2)
        verify(syncPage.advancedExpanded)
        const categoryIndex = syncPage.categories.findIndex(category => category.keys.indexOf("theme/reducedMotion")
                                                                        >= 0)

        verify(categoryIndex >= 0)
        syncPage.activateRow(3 + categoryIndex)
        tryVerify(() => !state("theme/reducedMotion").enabled)
        tryVerify(() => SettingsSync.customized)
        capture("sync-custom-categories")
        syncPage.visible = false
        settingsPage.visible = true
        settingsPage.activateRow(settingsPage.currentRow(), settingsPage.currentIndex)
        cycle()
        compare(Settings.values["theme/reducedMotion"], false)
        compare(record("first", "theme/reducedMotion"), true)
        SettingsSync.resetSettingOverrides()
        cycle()
        verify(!SettingsSync.customized)
        verify(state("theme/reducedMotion").enabled)
        compare(Settings.values["theme/reducedMotion"], true)
        verify(!state("playback/maxStreamingHeight").enabled)

        // Account replacement during a delayed HTTP snapshot cannot apply A's
        // in-flight response to B or alter the explicitly selected sync source.
        Integration.setReadDelay(350)
        SettingsSync.retry()
        tryVerify(() => SettingsSync.busy)
        SettingsSync.setAccountId(Integration.second)
        if (SettingsSync.accountChangePending)
            SettingsSync.confirmAccountChange(true)
        // The cancelled first-account cycle can finish before the debounced
        // second-account cycle starts. Wait for B's applied snapshot, not that
        // temporary idle interval.
        tryVerify(() => SettingsSync.accountId === Integration.second && !SettingsSync.busy
                        && Settings.values["audio/trackMode"] === "Default" && state("audio/trackMode").status
                        === "synced", 10000)
        Integration.setReadDelay(0)
        compare(Settings.values["audio/trackMode"], "Default")
        wait(400)
        compare(Settings.values["audio/trackMode"], "Default")

        // Both real settings surfaces, three viewports, two zoom levels and
        // confirmed/pending/error/opt-out states, without a desktop window.
        for (const size of [[1280, 720], [1920, 1080], [3840, 2160]]) {
            testCase.width = size[0]
            testCase.height = size[1]
            testCase.Window.window.width = size[0]
            testCase.Window.window.height = size[1]
            for (const zoom of [100, 150]) {
                Settings.setValue("appearance/uiScalePercent", zoom)
                tryCompare(Settings, "uiScalePercent", zoom)
                for (const status of ["synced", "pending", "error", "off"]) {
                    Integration.setStorageFailure(false)
                    SettingsSync.setSettingEnabled("theme/reducedMotion", true)
                    SettingsSync.setSettingEnabled("subtitles/scalePercent", true)
                    cycle()
                    if (status === "off") {
                        SettingsSync.setSettingEnabled("theme/reducedMotion", false)
                        SettingsSync.setSettingEnabled("subtitles/scalePercent", false)
                    } else if (status === "error") {
                        Integration.setStorageFailure(true)
                        cycle()
                    } else if (status === "pending") {
                        SettingsSync.beginEdit("theme/reducedMotion")
                        Settings.setValue("theme/reducedMotion", !Settings.values["theme/reducedMotion"])
                        SettingsSync.beginEdit("subtitles/scalePercent")
                        Settings.setValue("subtitles/scalePercent", Settings.values["subtitles/scalePercent"] === 100
                                          ? 110 : 100)
                    }
                    tryVerify(() => state("theme/reducedMotion").status === status, 10000, JSON.stringify(state(
                                                                                                              "theme/reducedMotion")))
                    tryVerify(() => state("subtitles/scalePercent").status === status, 10000, JSON.stringify(state(
                                                                                                                 "subtitles/scalePercent")))
                    const suffix = size[0] + "x" + size[1] + "-" + zoom + "-" + status
                    settingsPage.visible = true
                    subtitlePage.visible = false
                    selectSetting("theme/reducedMotion")
                    capture("settings-" + suffix)
                    settingsPage.visible = false
                    syncPage.visible = true
                    capture("sync-" + suffix)
                    syncPage.visible = false
                    subtitlePage.visible = true
                    capture("subtitles-" + suffix)
                    if (status === "pending") {
                        SettingsSync.endEdit("theme/reducedMotion", true)
                        SettingsSync.endEdit("subtitles/scalePercent", true)
                    }
                }
            }
        }
        Integration.setStorageFailure(false)
        subtitlePage.visible = false
        remotePage.visible = true
        RemoteTargets.setChooserVisible(true)
        tryVerify(() => !RemoteTargets.busy && RemoteTargets.targets.length >= 3, 10000)
        const target = RemoteTargets.targets.find(row => row.accountId === Integration.first && !row.isLocal)
        verify(target)
        remotePage.focusedKey = remotePage.rows.find(row => row.targetId === target.id).key
        remotePage.activate()
        tryVerify(() => RemoteTargets.selectedTargetId === target.id && !RemoteTargets.busy, 10000)
        compare(Integration.stats("first").queue.length, 2)
        remotePage.focusedKey = "queue"
        remotePage.activate()
        tryVerify(() => RemoteTargets.queue.length === 2 && !RemoteTargets.queueBusy, 10000)
        const remove = remotePage.rows.find(row => row.command && row.command.action === "queueRemove" && row.entryId
                                                   === "two")

        verify(remove)
        remotePage.focusedKey = remove.key
        remotePage.activate()
        tryVerify(() => Integration.stats("first").queue.length === 1, 10000)
        compare(Integration.stats("first").queue[0].entryId, "one")
        RemoteTargets.disconnectTarget()
        remotePage.visible = false

        Integration.startProtected()
        tryVerify(() => Integration.picker !== null, 10000)
        surface.context = Integration.picker
        tryVerify(() => surface.screen !== null, 10000)
        const pin = findChild(surface.screen, "integrationPin")
        const submit = findChild(surface.screen, "integrationSubmit")
        verify(pin && submit)
        pin.text = "bad"
        mouseClick(submit)
        tryVerify(() => surface.context.closed, 10000)
        verify(!Integration.running(Integration.protectedAccount))
        verify(Integration.running(Integration.first))
        surface.context = null
        tryVerify(() => Providers.accounts.find(account => account.id === Integration.protectedAccount).connectionState
                        === "locked", 10000)
        Integration.startProtected()
        tryVerify(() => Integration.picker !== null && !Integration.picker.closed, 10000)
        surface.context = Integration.picker
        tryVerify(() => surface.screen !== null, 10000)
        findChild(surface.screen, "integrationPin").text = "1234"
        mouseClick(findChild(surface.screen, "integrationSubmit"))
        tryVerify(() => Integration.running(Integration.protectedAccount), 10000)
        surface.context = null
        remotePage.visible = false
        settingsPage.visible = false
        subtitlePage.visible = false
        syncPage.visible = true
        Metrics.keyboardFocusActive = true
        SettingsSync.setEnabled(false)
        syncPage.forceActiveFocus()
        syncPage.focusEntry()
        const recovery = findChild(syncPage, "syncRecovery")
        const syncList = findChild(syncPage, "syncSettingsList")
        tryCompare(recovery, "activeFocus", true)
        syncPage.routeKey(Qt.Key_Down, "press", false)
        tryCompare(syncList, "activeFocus", true)
        syncList.currentIndex = 0
        syncPage.routeKey(Qt.Key_Up, "press", false)
        tryCompare(recovery, "activeFocus", true)
        syncPage.activate()
        tryVerify(() => SettingsSync.enabled)
        cycle()
        capture("sync-recovered")
    }
}
