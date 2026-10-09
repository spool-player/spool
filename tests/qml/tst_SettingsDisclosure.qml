import QtQuick
import QtTest
import "../../qml/primitives" as Primitives
import "../../qml/pages/SettingsNavigation.js" as SettingsNavigation

TestCase {
    id: testCase
    name: "SettingsDisclosure"
    width: 360
    height: 240
    when: windowShown

    property int containmentCount: 0
    readonly property string disclosureKey: "action/toggleAdvanced/Playback"

    ListModel {
        id: rowsModel
    }

    Primitives.MenuListView {
        id: settingsList
        anchors.fill: parent
        model: rowsModel
        delegate: Rectangle {
            required property int index
            required property string rowKey
            width: settingsList.width
            height: 50
        }
    }

    SignalSpy {
        id: currentIndexSpy
        target: settingsList
        signalName: "currentIndexChanged"
    }

    SignalSpy {
        id: activeFocusSpy
        target: settingsList
        signalName: "activeFocusChanged"
    }

    function collapsedRows() {
        return [
                    {
                        "rowKey": "appearance/scale",
                        "showHeader": true,
                        "sourceIndex": 0
                    },
                    {
                        "rowKey": "playback/basic",
                        "showHeader": true,
                        "sourceIndex": 10
                    },
                    {
                        "rowKey": disclosureKey,
                        "showHeader": false,
                        "sourceIndex": 11
                    },
                    {
                        "rowKey": "audio/basic",
                        "showHeader": true,
                        "sourceIndex": 20
                    }
                ]
    }

    function expandedRows() {
        const rows = collapsedRows()
        rows.splice(3, 0, {
                        "rowKey": "playback/advancedOne",
                        "showHeader": false,
                        "sourceIndex": 12
                    }, {
                        "rowKey": "playback/advancedTwo",
                        "showHeader": false,
                        "sourceIndex": 14
                    })
        return rows
    }

    function contain(index) {
        ++containmentCount
        settingsList.positionViewAtIndex(index, ListView.Contain)
    }

    function settleDisclosure(nextRows) {
        const disclosureIndex = SettingsNavigation.indexForRowKey(rowsModel, disclosureKey)
        settingsList.autoPositionCurrentItem = false
        settingsList.currentIndex = disclosureIndex
        settingsList.forceActiveFocus()
        SettingsNavigation.reconcileRows(rowsModel, nextRows)
        const settledIndex = SettingsNavigation.indexForRowKey(rowsModel, disclosureKey)
        settingsList.currentIndex = settledIndex
        settingsList.forceLayout()
        settingsList.autoPositionCurrentItem = true
        contain(settledIndex)
    }

    function init() {
        rowsModel.clear()
        const rows = expandedRows()
        for (let index = 0; index < rows.length; ++index)
            rowsModel.append(rows[index])
        settingsList.forceLayout()
        settingsList.currentIndex = 4
        settingsList.forceActiveFocus()
        wait(0)
        containmentCount = 0
        currentIndexSpy.clear()
        activeFocusSpy.clear()
    }

    function test_collapseSelectsDisclosureBeforeRemovalAndKeepsFocus() {
        settleDisclosure(collapsedRows())
        compare(rowsModel.count, 4)
        compare(settingsList.currentIndex, 2)
        compare(rowsModel.get(settingsList.currentIndex).rowKey, disclosureKey)
        verify(settingsList.activeFocus)
        compare(activeFocusSpy.count, 0)
        compare(containmentCount, 1)
        for (let index = 0; index < currentIndexSpy.count; ++index)
            verify(currentIndexSpy.signalArguments[index][0] !== 0)
    }

    function test_expansionKeepsDisclosureSelectedAndContainsOnce() {
        settleDisclosure(collapsedRows())
        containmentCount = 0
        currentIndexSpy.clear()
        settleDisclosure(expandedRows())
        compare(rowsModel.count, 6)
        compare(settingsList.currentIndex, 2)
        compare(rowsModel.get(settingsList.currentIndex).rowKey, disclosureKey)
        verify(settingsList.activeFocus)
        compare(containmentCount, 1)
    }

    function test_offscreenAnchorRecoveryGesture_data() {
        return [
                    {
                        tag: "right",
                        key: Qt.Key_Right
                    },
                    {
                        tag: "left",
                        key: Qt.Key_Left
                    },
                    {
                        tag: "down",
                        key: Qt.Key_Down
                    },
                    {
                        tag: "up",
                        key: Qt.Key_Up
                    }
                ]
    }

    function test_offscreenAnchorRecoveryGesture(data) {
        for (let index = 0; index < 8; ++index)
            rowsModel.append({
                                 "rowKey": "extra/" + index,
                                 "showHeader": false,
                                 "sourceIndex": 30 + index
                             })
        settingsList.forceLayout()
        settingsList.currentIndex = 0
        settingsList.contentY = 200
        wait(0)
        const viewportY = settingsList.contentY
        const gesture = {
            "key": 0
        }
        verify(SettingsNavigation.consumeRecoveryGesture(settingsList, testCase, Primitives.InputKeys, gesture, data.key,
                                                         "press", false))
        compare(settingsList.contentY, viewportY)
        verify(settingsList.currentIndex > 0)
        verify(settingsList.activeFocus)
        const recoveredIndex = settingsList.currentIndex
        verify(SettingsNavigation.consumeRecoveryGesture(settingsList, testCase, Primitives.InputKeys, gesture, data.key,
                                                         "press", true))
        verify(SettingsNavigation.consumeRecoveryGesture(settingsList, testCase, Primitives.InputKeys, gesture, data.key,
                                                         "release", true))
        verify(SettingsNavigation.consumeRecoveryGesture(settingsList, testCase, Primitives.InputKeys, gesture, data.key,
                                                         "press", true))
        compare(settingsList.currentIndex, recoveredIndex)
        compare(settingsList.contentY, viewportY)
        verify(SettingsNavigation.consumeRecoveryGesture(settingsList, testCase, Primitives.InputKeys, gesture, data.key,
                                                         "release", false))
        // A fresh physical gesture now navigates/edits from the recovered row.
        verify(!SettingsNavigation.consumeRecoveryGesture(settingsList, testCase, Primitives.InputKeys, gesture,
                                                          data.key, "press", false))
    }
}
