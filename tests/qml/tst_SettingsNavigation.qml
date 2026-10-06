import QtQuick
import QtTest
import "../../qml/pages/SettingsNavigation.js" as SettingsNavigation

TestCase {
    name: "SettingsNavigation"

    function valueLookup(values) {
        return function (key) {
            return values[key]
        }
    }

    function test_progressiveDisclosureLevels() {
        compare(SettingsNavigation.detailLevel({}), 0)
        compare(SettingsNavigation.detailLevel({
                                                   "level": 1
                                               }), 1)
        compare(SettingsNavigation.detailLevel({
                                                   "level": 2
                                               }), 2)
    }

    function test_platformAndDependencyFiltering() {
        const values = {
            "playback/mpvConfigMode": "custom"
        }
        const lookup = valueLookup(values)
        // Stands in for the Platform singleton. webOS and Android TV are both
        // televisions, so isTV alone cannot tell a webOS-only row from an
        // Android one -- which is why each platform is asked about directly.
        const desktop = {
            "isTV": false,
            "isWebOS": false,
            "isAndroid": false
        }
        const webos = {
            "isTV": true,
            "isWebOS": true,
            "isAndroid": false
        }
        const androidTv = {
            "isTV": true,
            "isWebOS": false,
            "isAndroid": true
        }
        const androidPhone = {
            "isTV": false,
            "isWebOS": false,
            "isAndroid": true
        }

        verify(SettingsNavigation.rowAvailable({
                                                   "platform": "desktop"
                                               }, desktop, false, lookup))
        verify(!SettingsNavigation.rowAvailable({
                                                    "platform": "desktop"
                                                }, webos, false, lookup))
        verify(SettingsNavigation.rowAvailable({
                                                   "platform": "webos"
                                               }, webos, false, lookup))
        verify(!SettingsNavigation.rowAvailable({
                                                    "platform": "webos"
                                                }, desktop, false, lookup))
        // A webOS row must not leak onto Android TV just because it is a
        // television.
        verify(!SettingsNavigation.rowAvailable({
                                                    "platform": "webos"
                                                }, androidTv, false, lookup))
        verify(SettingsNavigation.rowAvailable({
                                                   "platform": "android"
                                               }, androidPhone, false, lookup))
        verify(SettingsNavigation.rowAvailable({
                                                   "platform": "android"
                                               }, androidTv, false, lookup))
        verify(!SettingsNavigation.rowAvailable({
                                                    "platform": "android"
                                                }, desktop, false, lookup))
        verify(SettingsNavigation.rowAvailable({
                                                   "dependsOnKey": "playback/mpvConfigMode",
                                                   "dependsOnValue": "custom"
                                               }, desktop, false, lookup))
        verify(!SettingsNavigation.rowAvailable({
                                                    "dependsOnKey": "playback/mpvConfigMode",
                                                    "dependsOnValue": "standard"
                                                }, desktop, false, lookup))
        verify(!SettingsNavigation.rowAvailable(null, desktop, false, lookup))
        verify(!SettingsNavigation.rowAvailable({
                                                    "requiresHdrPlayback": true
                                                }, desktop, false, lookup))
        verify(SettingsNavigation.rowAvailable({
                                                   "requiresHdrPlayback": true
                                               }, desktop, true, lookup))
    }

    function test_focusClampsAfterFiltering() {
        compare(SettingsNavigation.clampIndex(-1, 5), 0)
        compare(SettingsNavigation.clampIndex(3, 5), 3)
        compare(SettingsNavigation.clampIndex(9, 5), 4)
        compare(SettingsNavigation.clampIndex(4, 2), 1)
        compare(SettingsNavigation.clampIndex(0, 0), -1)
    }

    function test_stableKeyLookupSupportsArraysAndListModels() {
        const rows = [
                  {
                      "rowKey": "a"
                  },
                  {
                      "rowKey": "b"
                  }
              ]
        compare(SettingsNavigation.indexForRowKey(rows, "b"), 1)
        compare(SettingsNavigation.indexForRowKey(rows, "missing"), -1)

        const model = Qt.createQmlObject("import QtQuick; ListModel {}", this)
        model.append(rows[0])
        model.append(rows[1])
        compare(SettingsNavigation.indexForRowKey(model, "a"), 0)
        model.destroy()
    }

    function test_nearestSourceOrderPrefersPrecedingRowOnTie() {
        const rows = [
                  {
                      "rowKey": "before",
                      "sourceIndex": 8
                  },
                  {
                      "rowKey": "after",
                      "sourceIndex": 12
                  }
              ]
        compare(SettingsNavigation.nearestRowKey(rows, 10), "before")
        compare(SettingsNavigation.nearestRowKey(rows, 11), "after")
        compare(SettingsNavigation.nearestRowKey([], 10), "")
    }

    function test_reconcileRowsPreservesPrefixAndSuffix() {
        const model = Qt.createQmlObject("import QtQuick; ListModel {}", this)
        model.append({
                         "rowKey": "prefix",
                         "showHeader": true,
                         "sourceIndex": 0
                     })
        model.append({
                         "rowKey": "remove",
                         "showHeader": false,
                         "sourceIndex": 2
                     })
        model.append({
                         "rowKey": "suffix",
                         "showHeader": false,
                         "sourceIndex": 6
                     })
        SettingsNavigation.reconcileRows(model, [
                                             {
                                                 "rowKey": "prefix",
                                                 "showHeader": true,
                                                 "sourceIndex": 0
                                             },
                                             {
                                                 "rowKey": "insert-one",
                                                 "showHeader": false,
                                                 "sourceIndex": 2
                                             },
                                             {
                                                 "rowKey": "insert-two",
                                                 "showHeader": false,
                                                 "sourceIndex": 4
                                             },
                                             {
                                                 "rowKey": "suffix",
                                                 "showHeader": true,
                                                 "sourceIndex": 6
                                             }
                                         ])
        compare(model.count, 4)
        compare(model.get(0).rowKey, "prefix")
        compare(model.get(1).rowKey, "insert-one")
        compare(model.get(2).rowKey, "insert-two")
        compare(model.get(3).rowKey, "suffix")
        compare(model.get(3).showHeader, true)
        model.destroy()
    }

    function test_valueRouting_data() {
        return [
                    {
                        tag: "right-adjusts",
                        mode: "row",
                        action: "right",
                        edit: true,
                        next: "row",
                        effect: "value"
                    },
                    {
                        tag: "left-adjusts",
                        mode: "row",
                        action: "left",
                        edit: true,
                        next: "row",
                        effect: "value"
                    },
                    {
                        tag: "begin-edit",
                        mode: "row",
                        action: "activate",
                        edit: true,
                        next: "value-editing",
                        effect: "begin-edit"
                    },
                    {
                        tag: "editing-right",
                        mode: "value-editing",
                        action: "right",
                        edit: true,
                        next: "value-editing",
                        effect: "value"
                    },
                    {
                        tag: "finish-edit",
                        mode: "value-editing",
                        action: "back",
                        edit: true,
                        next: "row",
                        effect: "end-edit"
                    },
                    {
                        tag: "move-row",
                        mode: "value-editing",
                        action: "down",
                        edit: true,
                        next: "row",
                        effect: "move-down"
                    },
                    {
                        tag: "activate-toggle",
                        mode: "row",
                        action: "activate",
                        edit: false,
                        next: "row",
                        effect: "activate"
                    }
                ]
    }

    function test_valueRouting(data) {
        const result = SettingsNavigation.valueRoute(data.mode, data.action, data.edit)
        compare(result.mode, data.next)
        compare(result.effect, data.effect)
    }
}
