import QtQuick
import QtTest
import "../../qml/pages/SettingsNavigation.js" as SettingsNavigation
import "../../qml/primitives" as Primitives
import "../../qml/pages" as Pages
import "../../qml/theme" as ThemeModule

TestCase {
    id: testCase
    name: "SubtitleSettingsPanel"
    width: 1280
    height: 720
    visible: true
    when: windowShown

    QtObject {
        id: settingsFixture
        property var values: ({})
        property var previews: ({})
        property int commits: 0
        property var settingsSchema: [
            {
                key: "subtitles/scalePercent",
                title: "Size",
                type: "slider",
                from: 50,
                to: 200,
                step: 5,
                defaultValue: 100,
                syncPolicy: "portable"
            },
            {
                key: "subtitles/verticalPositionPercent",
                title: "Position",
                type: "slider",
                from: 0,
                to: 100,
                step: 1,
                defaultValue: 50,
                syncPolicy: "portable"
            },
            {
                key: "subtitles/recolorImageSubtitles",
                title: "Recolour image subtitles",
                type: "toggle",
                defaultValue: false,
                syncPolicy: "portable"
            },
            {
                key: "subtitles/bitmapSharpnessPercent",
                title: "Sharpness",
                type: "slider",
                from: 0,
                to: 100,
                step: 1,
                defaultValue: 50,
                syncPolicy: "portable",
                dependsOnKey: "subtitles/recolorImageSubtitles",
                dependsOnValue: true
            },
            {
                key: "subtitles/hdrBrightnessPercent",
                title: "HDR brightness",
                type: "slider",
                from: 50,
                to: 200,
                step: 5,
                defaultValue: 100,
                syncPolicy: "device",
                requiresHdrPlayback: true
            }
        ]
        function setValue(key, value) {
            ++commits
            const next = Object.assign({}, values)
            next[key] = value
            values = next
        }
        function previewValue(key, value) {
            const next = Object.assign({}, previews)
            next[key] = value
            previews = next
        }
    }

    QtObject {
        id: syncFixture
        property var states: ({})
        property int begins: 0
        property int ends: 0
        property string editingKey: ""
        property bool lastChanged: false
        property var deferredValue: undefined
        function beginEdit(key) {
            ++begins
            editingKey = key
        }
        function endEdit(key, changed) {
            ++ends
            lastChanged = changed
            editingKey = ""
            if (!changed && deferredValue !== undefined) {
                const next = Object.assign({}, settingsFixture.values)
                next[key] = deferredValue
                settingsFixture.values = next
            }
            deferredValue = undefined
        }
        function setSettingEnabled(key, enabled) {
            const next = Object.assign({}, states)
            next[key] = Object.assign({}, states[key], {
                                          enabled: enabled,
                                          status: enabled ? "pending" : "off"
                                      })
            states = next
        }
    }

    Component {
        id: panelComponent
        Pages.SubtitleSettingsPanel {
            width: testCase.width
            height: testCase.height
            settingsController: settingsFixture
            syncController: syncFixture
            platformInfo: ({
                               isTV: false,
                               isWebOS: false,
                               isAndroid: false,
                               hasSystemFonts: false
                           })
            hdrPlayback: false
        }
    }

    function init() {
        ThemeModule.Metrics.keyboardFocusActive = true
        settingsFixture.values = {
            "subtitles/scalePercent": 100,
            "subtitles/verticalPositionPercent": 50,
            "subtitles/recolorImageSubtitles": false,
            "subtitles/bitmapSharpnessPercent": 50,
            "subtitles/hdrBrightnessPercent": 100
        }
        settingsFixture.previews = {}
        settingsFixture.commits = 0
        syncFixture.begins = 0
        syncFixture.ends = 0
        syncFixture.editingKey = ""
        syncFixture.lastChanged = false
        syncFixture.deferredValue = undefined
        syncFixture.states = {
            "subtitles/scalePercent": {
                eligible: true,
                enabled: true,
                backend: "spool",
                status: "synced",
                help: "Spool-specific sync"
            },
            "subtitles/verticalPositionPercent": {
                eligible: true,
                enabled: true,
                backend: "spool",
                status: "synced"
            }
        }
    }

    function createPanel() {
        const panel = createTemporaryObject(panelComponent, testCase)
        verify(panel)
        panel.forceActiveFocus()
        panel.focusRow(1)
        tryVerify(function () {
            return panel.rowControlAt(1) !== null
        })
        return panel
    }

    function press(panel, key) {
        verify(panel.routeKey(key, "press", false))
        verify(panel.routeKey(key, "release", false))
    }

    function test_horizontalKeysEditValuesWithoutSyncSubfocus() {
        const panel = createPanel()
        press(panel, Qt.Key_Right)
        compare(settingsFixture.values["subtitles/scalePercent"], 105)
        compare(panel.navigationMode, "row")
        press(panel, Qt.Key_Down)
        compare(panel.currentRowIndex, 2)
        press(panel, Qt.Key_Right)
        compare(settingsFixture.values["subtitles/verticalPositionPercent"], 51)
        press(panel, Qt.Key_Left)
        compare(settingsFixture.values["subtitles/verticalPositionPercent"], 50)
        verify(syncFixture.states["subtitles/scalePercent"].enabled)
    }

    function test_sliderEditDefersRemoteUntilExitAndKeepsHorizontalKeys() {
        const panel = createPanel()
        press(panel, Qt.Key_Return)
        compare(syncFixture.editingKey, "subtitles/scalePercent")
        syncFixture.deferredValue = 160
        press(panel, Qt.Key_Right)
        compare(panel.navigationMode, "value-editing")
        compare(settingsFixture.values["subtitles/scalePercent"], 105)
        press(panel, Qt.Key_Escape)
        compare(panel.navigationMode, "row")
        compare(settingsFixture.values["subtitles/scalePercent"], 105)
        verify(syncFixture.lastChanged)
        compare(syncFixture.ends, 1)

        press(panel, Qt.Key_Return)
        syncFixture.deferredValue = 160
        press(panel, Qt.Key_Return)
        compare(settingsFixture.values["subtitles/scalePercent"], 160)
        verify(!syncFixture.lastChanged)
        compare(syncFixture.ends, 2)
    }

    function test_pointerSliderPreviewsThenCommits() {
        const panel = createPanel()
        const row = panel.rowControlAt(1)
        const slider = row.trailing[0]
        mousePress(slider, slider.width * 0.8, slider.height / 2)
        compare(settingsFixture.values["subtitles/scalePercent"], 100)
        compare(settingsFixture.previews["subtitles/scalePercent"], 170)
        compare(settingsFixture.commits, 0)
        compare(syncFixture.editingKey, "subtitles/scalePercent")
        mouseMove(slider, slider.width * 0.6, slider.height / 2)
        compare(settingsFixture.previews["subtitles/scalePercent"], 140)
        mouseRelease(slider, slider.width * 0.6, slider.height / 2)
        compare(settingsFixture.values["subtitles/scalePercent"], 140)
        compare(settingsFixture.commits, 1)
        compare(syncFixture.begins, 1)
        compare(syncFixture.ends, 1)
        verify(syncFixture.lastChanged)
    }

    function test_reachabilityIncludesCollapsedAdvancedButNotHiddenValues() {
        const panel = createPanel()
        verify(panel.reachableSettingKeys.indexOf("subtitles/recolorImageSubtitles") >= 0)
        verify(panel.reachableSettingKeys.indexOf("subtitles/bitmapSharpnessPercent") < 0)
        verify(panel.reachableSettingKeys.indexOf("subtitles/hdrBrightnessPercent") < 0)
        settingsFixture.setValue("subtitles/recolorImageSubtitles", true)
        verify(panel.reachableSettingKeys.indexOf("subtitles/bitmapSharpnessPercent") >= 0)
        panel.hdrPlayback = true
        verify(panel.reachableSettingKeys.indexOf("subtitles/hdrBrightnessPercent") >= 0)
    }

    readonly property var sections: [
        {
            "title": "Track",
            "keys": ["subtitles/language", "subtitles/mode"]
        },
        {
            "title": "Image subtitles",
            "keys": ["subtitles/recolorImageSubtitles", "subtitles/bitmapSharpnessPercent"]
        },
        {
            "title": "HDR",
            "keys": ["subtitles/hdrBrightnessPercent"]
        }
    ]

    Component {
        id: menuListComponent

        Primitives.MenuListView {
            width: 320
            height: 240
            delegate: Item {
                required property int index
                width: 320
                height: 20
            }
        }
    }

    function resolveAll(key) {
        return {
            "key": key
        }
    }

    function test_sectionsFlattenInDeclaredOrder() {
        const rows = SettingsNavigation.sectionedRows(sections, resolveAll)
        compare(rows.length, 8)
        verify(rows[0].section)
        compare(rows[0].spec.title, "Track")
        compare(rows[1].spec.key, "subtitles/language")
        compare(rows[2].spec.key, "subtitles/mode")
        verify(rows[3].section)
        compare(rows[3].spec.title, "Image subtitles")
        compare(rows[4].spec.key, "subtitles/recolorImageSubtitles")
    }

    // A header with nothing under it is worse than no header, so an empty
    // section has to disappear entirely.
    function test_emptySectionDropsItsHeader() {
        const rows = SettingsNavigation.sectionedRows(sections, function (key) {
            return key === "subtitles/hdrBrightnessPercent" ? null : resolveAll(key)
        })
        for (let index = 0; index < rows.length; ++index)
            verify(!rows[index].section || rows[index].spec.title !== "HDR")
        compare(rows.length, 6)
    }

    function test_missingRowsShrinkTheirSection() {
        const rows = SettingsNavigation.sectionedRows(sections, function (key) {
            return key === "subtitles/mode" ? null : resolveAll(key)
        })
        compare(rows[0].spec.title, "Track")
        compare(rows[1].spec.key, "subtitles/language")
        verify(rows[2].section)
    }

    // Section entries must stay unselectable, which MenuListView decides from
    // exactly this flag.
    function test_headersAreNotSelectable() {
        const rows = SettingsNavigation.sectionedRows(sections, resolveAll)
        const rowEnabled = function (entry) {
            return !(entry && entry.section === true)
        }
        verify(!rowEnabled(rows[0]))
        verify(rowEnabled(rows[1]))
    }

    function test_firstActionableRowSkipsAdvancedHeader() {
        const rows = SettingsNavigation.sectionedRows(sections, resolveAll)
        compare(SettingsNavigation.firstActionableRow(rows, 0), 1)
        compare(SettingsNavigation.firstActionableRow(rows, 3), 4)
        compare(SettingsNavigation.firstActionableRow([], 0), -1)
    }

    function test_menuListReadsSectionedArrayEntries() {
        const rows = SettingsNavigation.sectionedRows(sections, resolveAll)
        const list = createTemporaryObject(menuListComponent, testCase)
        verify(list)
        list.model = rows.length
        list.entryProvider = function (index) {
            return rows[index]
        }
        tryCompare(list, "count", rows.length)
        compare(list.entryAt(0).spec.title, "Track")
        verify(!list.isRowEnabled(0))
        verify(list.isRowEnabled(1))
        list.currentIndex = 1
        list.moveSelection(1)
        compare(list.currentIndex, 2)
        list.moveSelection(1)
        compare(list.currentIndex, 4)
        verify(list.routeKey(Qt.Key_Down, "release", false))
        compare(list.currentIndex, 4)
    }
    function test_noSectionsYieldsNoRows() {
        compare(SettingsNavigation.sectionedRows([], resolveAll).length, 0)
    }
}
