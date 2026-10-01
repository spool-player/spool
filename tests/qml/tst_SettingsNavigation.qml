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

    function test_syncSubfocusRouting_data() {
        return [
                    {
                        tag: "enter-icon",
                        mode: "row",
                        action: "right",
                        sync: true,
                        edit: true,
                        next: "sync-action",
                        effect: "none"
                    },
                    {
                        tag: "icon-does-not-advance",
                        mode: "sync-action",
                        action: "right",
                        sync: true,
                        edit: true,
                        next: "sync-action",
                        effect: "none"
                    },
                    {
                        tag: "toggle-only-sync",
                        mode: "sync-action",
                        action: "activate",
                        sync: true,
                        edit: true,
                        next: "sync-action",
                        effect: "toggle-sync"
                    },
                    {
                        tag: "leave-icon-left",
                        mode: "sync-action",
                        action: "left",
                        sync: true,
                        edit: true,
                        next: "row",
                        effect: "none"
                    },
                    {
                        tag: "leave-icon-back",
                        mode: "sync-action",
                        action: "back",
                        sync: true,
                        edit: true,
                        next: "row",
                        effect: "none"
                    },
                    {
                        tag: "icon-up-one-row",
                        mode: "sync-action",
                        action: "up",
                        sync: true,
                        edit: true,
                        next: "row",
                        effect: "move-up"
                    },
                    {
                        tag: "icon-down-one-row",
                        mode: "sync-action",
                        action: "down",
                        sync: true,
                        edit: true,
                        next: "row",
                        effect: "move-down"
                    },
                    {
                        tag: "begin-value-edit",
                        mode: "row",
                        action: "activate",
                        sync: true,
                        edit: true,
                        next: "value-editing",
                        effect: "begin-edit"
                    },
                    {
                        tag: "left-requires-edit",
                        mode: "row",
                        action: "left",
                        sync: true,
                        edit: true,
                        next: "row",
                        effect: "none"
                    },
                    {
                        tag: "edit-right-not-sync",
                        mode: "value-editing",
                        action: "right",
                        sync: true,
                        edit: true,
                        next: "value-editing",
                        effect: "value"
                    },
                    {
                        tag: "edit-left-not-sync",
                        mode: "value-editing",
                        action: "left",
                        sync: true,
                        edit: true,
                        next: "value-editing",
                        effect: "value"
                    },
                    {
                        tag: "finish-value-ok",
                        mode: "value-editing",
                        action: "activate",
                        sync: true,
                        edit: true,
                        next: "row",
                        effect: "end-edit"
                    },
                    {
                        tag: "finish-value-back",
                        mode: "value-editing",
                        action: "back",
                        sync: true,
                        edit: true,
                        next: "row",
                        effect: "end-edit"
                    },
                    {
                        tag: "edit-down-one-row",
                        mode: "value-editing",
                        action: "down",
                        sync: true,
                        edit: true,
                        next: "row",
                        effect: "move-down"
                    },
                    {
                        tag: "scale-still-adjusts",
                        mode: "row",
                        action: "right",
                        sync: false,
                        edit: true,
                        next: "row",
                        effect: "value"
                    },
                    {
                        tag: "missing-icon-restores-row",
                        mode: "sync-action",
                        action: "activate",
                        sync: false,
                        edit: true,
                        next: "value-editing",
                        effect: "begin-edit"
                    },
                    {
                        tag: "toggle-value",
                        mode: "row",
                        action: "activate",
                        sync: true,
                        edit: false,
                        next: "row",
                        effect: "activate"
                    }
                ]
    }

    function test_syncSubfocusRouting(data) {
        const result = SettingsNavigation.syncRoute(data.mode, data.action, data.sync, data.edit)
        compare(result.mode, data.next)
        compare(result.effect, data.effect)
    }

    function test_syncOnlyRowsIncludeHiddenValuesWithoutReachableDuplicates() {
        const platform = {
            isTV: false,
            isWebOS: false,
            isAndroid: false
        }
        const schema = [
                  {
                      key: "playback/maxStreamingBitrateMbps",
                      syncPolicy: "portable",
                      dependsOnKey: "playback/manualBitrate",
                      dependsOnValue: true
                  },
                  {
                      key: "subtitles/hdrBrightnessPercent",
                      syncPolicy: "device",
                      requiresHdrPlayback: true
                  },
                  {
                      key: "settings/audioDelayMs",
                      syncPolicy: "device"
                  },
                  {
                      key: "subtitles/scalePercent",
                      syncPolicy: "portable"
                  },
                  {
                      key: "theme/accent",
                      syncPolicy: "portable"
                  },
                  {
                      key: "appearance/uiScalePercent",
                      syncPolicy: "never"
                  },
                  {
                      key: "webos/redButton",
                      syncPolicy: "device",
                      platform: "webos"
                  },
                  {
                      key: "future/unclassified"
                  }
              ]
        const subtitleReachable = SettingsNavigation.subtitleReachableKeys(schema, platform, false, valueLookup({
                                                                                                                    "playback/manualBitrate":
                                                                                                                    false
                                                                                                                }))
        compare(subtitleReachable, ["subtitles/scalePercent"])
        const extra = SettingsNavigation.extraSyncRows(schema, platform, subtitleReachable.concat(["theme/accent"]))
        compare(extra.map(function (row) {
            return row.key
        }), ["playback/maxStreamingBitrateMbps", "subtitles/hdrBrightnessPercent", "settings/audioDelayMs"])
        const hdrReachable = SettingsNavigation.subtitleReachableKeys(schema, platform, true, valueLookup({}))
        const withHdr = SettingsNavigation.extraSyncRows(schema, platform, hdrReachable.concat(["theme/accent",
                                                                                                "playback/maxStreamingBitrateMbps"]))
        compare(withHdr.map(function (row) {
            return row.key
        }), ["settings/audioDelayMs"])
    }

    function test_losingSyncSubfocusKeepsStableRowIdentity() {
        const model = Qt.createQmlObject("import QtQuick; ListModel {}", this)
        model.append({
                         rowKey: "audio/language",
                         sourceIndex: 4
                     })
        model.append({
                         rowKey: "theme/accent",
                         sourceIndex: 6
                     })
        const selectedKey = model.get(1).rowKey
        const nextMode = SettingsNavigation.normalizeSyncMode("sync-action", false)
        SettingsNavigation.reconcileRows(model, [
                                             {
                                                 rowKey: "audio/language",
                                                 sourceIndex: 4
                                             },
                                             {
                                                 rowKey: "theme/accent",
                                                 sourceIndex: 6
                                             }
                                         ])
        compare(nextMode, "row")
        compare(SettingsNavigation.indexForRowKey(model, selectedKey), 1)
        model.destroy()
    }
}
