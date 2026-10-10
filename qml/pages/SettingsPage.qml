pragma ComponentBehavior: Bound

import QtQuick
import "../theme"
import "../primitives"
import "SettingsNavigation.js" as SettingsNavigation

FocusScope {
    id: root

    property var shell
    property var uiTransitionToken: 0
    property int currentIndex: -1
    property string selectedRowKey: ""
    property var rowsByKey: ({})
    property var allSettingsRows: []
    property var expandedGroups: ({})
    property bool reconcilingSettingsRows: false
    readonly property bool directionRelease: true
    property var recoveryGesture: ({
                                       "key": 0
                                   })
    property string pageMode: "index"
    property string categoryId: ""
    property string searchQuery: ""
    property var priorSearchLocation: null
    property var searchResultLocation: null
    property var categoryLocations: ({})
    property string revealedRowKey: ""
    readonly property string pageTitle: pageMode === "search" ? "Search settings" : pageMode === "category"
                                                                ? SettingsNavigation.categoryTitle(categoryId) :
                                                                  "Settings"
    readonly property bool modalVisible: certificateManagerVisible || choiceDialogVisible || diagnosticsExportVisible
                                         || mpvFolderDialogVisible
    property bool mpvFolderDialogVisible: false
    onModalVisibleChanged: if (!modalVisible && rowsBuilt)
                               Qt.callLater(function () {
                                   refreshSettingsFilter(false)
                               })

    function inputCompositionActive() {
        const focused = root.Window.window ? root.Window.window.activeFocusItem : null
        return Boolean(focused && focused.inputMethodComposing)
    }

    function location() {
        return {
            "mode": pageMode,
            "categoryId": categoryId,
            "rowKey": selectedRowKey,
            "scrollOffset": settingsList.contentY - settingsList.originY,
            "query": searchQuery,
            "expanded": expandedGroups,
            "revealedRowKey": revealedRowKey
        }
    }

    function restoreLocation(saved) {
        if (!saved)
            return
        finishRowEdit()
        navigationMode = "row"
        pageMode = saved.mode
        categoryId = saved.categoryId
        searchQuery = saved.query
        expandedGroups = saved.expanded
        revealedRowKey = saved.revealedRowKey
        reconcileSettingsRows(rebuildVisibleRows(), saved.rowKey, true)
        // Incremental reconciliation can move ListView's content origin while
        // retaining the result delegate. Raw contentY belongs to the old model.
        settingsList.contentY = settingsList.originY + saved.scrollOffset
    }

    function openSearch() {
        if (modalVisible || editingKey.length || inputCompositionActive())
            return false
        if (pageMode !== "search")
            priorSearchLocation = location()
        searchResultLocation = null
        pageMode = "search"
        refreshSettingsFilter(true)
        Qt.callLater(searchField.focusField)
        return true
    }

    function openCategory(id, rowKey) {
        finishRowEdit()
        navigationMode = "row"
        if (pageMode === "category")
            categoryLocations[categoryId] = location()
        if (!rowKey && categoryLocations[id]) {
            restoreLocation(categoryLocations[id])
            return
        }
        categoryId = id
        pageMode = "category"
        revealedRowKey = rowKey || ""
        const row = rowsByKey[rowKey]
        if (row && SettingsNavigation.detailLevel(row) > 0) {
            const next = Object.assign({}, expandedGroups)
            next[id] = true
            expandedGroups = next
        }
        const nextRows = rebuildVisibleRows()
        reconcileSettingsRows(nextRows, rowKey || (nextRows.length ? nextRows[0].rowKey : ""), true)
        if (rowKey) {
            const index = SettingsNavigation.indexForRowKey(settingsRows, rowKey)
            // Put the preceding neighbour on screen before containing the target.
            if (index > 0)
                settingsList.positionViewAtIndex(index - 1, ListView.Beginning)
            if (index >= 0)
                settingsList.positionViewAtIndex(index, ListView.Contain)
        } else {
            settingsList.positionViewAtBeginning()
        }
    }

    function availabilityReason(row) {
        if (!SettingsNavigation.platformSupported(row, Platform))
            return "Unavailable on this device (" + row.platform + " only)"
        if (row.requiresHdrPlayback && !Player.hdrPlayback)
            return "Available during HDR playback"
        if (row.destination === "player" && !Player.sessionActive)
            return "Available in the player during playback"
        if (row.dependsOnKey && String(settingsValue(rowsByKey[row.dependsOnKey] || {
                                                         "key": row.dependsOnKey
                                                     })) !== String(row.dependsOnValue)) {
            const prerequisite = rowsByKey[row.dependsOnKey]
            return "Requires " + (prerequisite ? prerequisite.title : row.dependsOnKey) + ": " + row.dependsOnValue
                    + ". Open that setting to change it."
        }
        if (!rowAvailable(row))
            return row.categoryId === "downloads"
                    ? "Available when downloads and folder selection are supported and enabled" :
                      "Not supported by the active providers"
        return ""
    }

    function openSearchResult(row) {
        searchResultLocation = location()
        openCategory(row.categoryId, row.key)
    }

    function unhandledKey(key, phase, repeat, modifiers) {
        const focused = root.Window.window ? root.Window.window.activeFocusItem : null
        if (key !== Qt.Key_F || !(modifiers & Qt.ControlModifier) || InputKeys.isTextInputItem(focused) || modalVisible
                || editingKey.length || inputCompositionActive())
            return false
        return phase !== "press" || repeat || openSearch()
    }
    property bool choiceDialogVisible: false
    property var choiceDialogRow: null
    property Item choiceDialogAnchor: null
    readonly property var choiceDialog: choiceDialogLoader.item
    readonly property var mpvFolderDialog: mpvFolderDialogLoader.item
    // Rows are rebuilt whenever the settings tree changes -- a disclosure
    // opens, the advanced filter flips -- and readiness used to be unset on
    // every rebuild, then set again only by a row delegate being constructed.
    // On a warm revisit the delegates already existed, so nothing set it
    // again and the page never reported itself settled: the route host waited
    // out its timeout on every visit here. Derived from the view now, so a
    // rebuild cannot strand it. No rows is a settled answer; rows are settled
    // once the first one is laid out.
    readonly property bool contentReady: settingsRows.count === 0 || settingsReveal.firstDelegateReady
    property bool certificateManagerVisible: false
    property bool diagnosticsExportVisible: false
    property string diagnosticsExportPreview: ""
    property bool pendingCustomMpvMode: false
    property string navigationMode: "row"
    property string editingKey: ""
    property var editingInitialValue
    onEditingKeyChanged: if (!editingKey.length && rowsBuilt)
                             Qt.callLater(function () {
                                 refreshSettingsFilter(false)
                             })
    function beginRowEdit(row) {
        if (!row || editingKey === row.key)
            return
        finishRowEdit()
        editingKey = row.key
        editingInitialValue = Settings.values[row.key]
        SettingsSync.beginEdit(row.key)
    }

    function finishRowEdit() {
        if (!editingKey.length)
            return
        const key = editingKey
        editingKey = ""
        SettingsSync.endEdit(key, Settings.values[key] !== editingInitialValue)
    }

    ListModel {
        id: settingsRows
    }

    readonly property bool downloadsAvailable: Downloads.supported && (Downloads.enabled || Downloads.jobs.length > 0)
    onDownloadsAvailableChanged: refreshSettingsFilter(false)

    function rowAvailable(row) {
        if (row.key === "action/connectionSpeed" && !ProviderCapabilities.speedTest)
            return false
        if (row.key === "action/downloads")
            return downloadsAvailable
        if (row.key === "action/downloadDestination")
            return Downloads.supported && Downloads.enabled && !Downloads.mobile && Downloads.canChooseFolder
        if (row.key === "action/mobileDownloadDestination")
            return Downloads.supported && Downloads.enabled && Downloads.mobile && Downloads.canChooseFolder
        return SettingsNavigation.rowAvailable(row, Platform, Player.hdrPlayback, function (key) {
            return settingsValue({
                                     "key": key,
                                     "defaultValue": ""
                                 })
        })
    }

    function disclosureKey(group) {
        return "action/toggleAdvanced/" + group
    }

    function groupExpanded(group) {
        return Boolean(expandedGroups[group])
    }

    function appendVisibleGroup(target, group, entries) {
        const essential = []
        const additional = []
        for (let index = 0; index < entries.length; ++index) {
            const entry = entries[index]
            const row = rowsByKey[entry.rowKey]
            if (!rowAvailable(row) && entry.rowKey !== revealedRowKey)
                continue
            if (entry.detailLevel === 0)
                essential.push(entry)
            else
                additional.push(entry)
        }
        if (essential.length === 0 && additional.length === 0)
            return
        let first = true
        for (let index = 0; index < essential.length; ++index) {
            const entry = essential[index]
            target.push({
                            "rowKey": entry.rowKey,
                            "showHeader": first,
                            "advanced": false,
                            "sourceIndex": entry.sourceIndex
                        })
            first = false
        }
        if (additional.length === 0)
            return
        const key = disclosureKey(group)
        target.push({
                        "rowKey": key,
                        "showHeader": first,
                        "advanced": false,
                        "sourceIndex": additional[0].sourceIndex - 1
                    })
        if (!groupExpanded(group))
            return
        for (let index = 0; index < additional.length; ++index)
            target.push({
                            "rowKey": additional[index].rowKey,
                            "advanced": true,
                            "showHeader": false,
                            "sourceIndex": additional[index].sourceIndex
                        })
    }

    function rebuildVisibleRows() {
        const visibleRows = []
        function descriptor(key, sourceIndex, header, advanced) {
            return {
                "rowKey": key,
                "sourceIndex": sourceIndex,
                "showHeader": Boolean(header),
                "advanced": Boolean(advanced)
            }
        }
        if (pageMode === "index") {
            visibleRows.push(descriptor("action/searchSettings", -2, false, false))
            visibleRows.push(descriptor("action/zoom", -1, false, false))
            for (let index = 0; index < SettingsNavigation.categories.length; ++index)
                visibleRows.push(descriptor("action/category/" + SettingsNavigation.categories[index].id, index, false,
                                            false))
            return visibleRows
        }
        const entries = []
        for (let index = 0; index < allSettingsRows.length; ++index) {
            const entry = allSettingsRows[index]
            const row = rowsByKey[entry.rowKey]
            if (pageMode === "search") {
                if (SettingsNavigation.matchesSearch(row, searchQuery, rowOptions(row)))
                    visibleRows.push(descriptor(row.key, entry.sourceIndex, false, false))
            } else if (entry.group === categoryId && (!row.destination || row.destination === "category" || row.key
                                                      === revealedRowKey)) {
                entries.push(entry)
            }
        }
        if (pageMode === "category")
            appendVisibleGroup(visibleRows, categoryId, entries)
        return visibleRows
    }

    // Search includes panel/player destinations, but category browsing retains
    // their existing editor entry points rather than duplicating controls.
    function buildSettingsRowsSource() {
        const schema = Settings.settingsSchema
        const rowMap = {}
        const sourceRows = []
        rowMap["action/settingsSync"] = {
            "key": "action/settingsSync",
            "categoryId": "sources",
            "group": "Sources",
            "title": "Settings sync",
            "description": "",
            "type": "action",
            "destination": "category"
        }
        rowMap["action/searchSettings"] = {
            "key": "action/searchSettings",
            "title": "Search settings",
            "description": "Find any setting by name, value or keyword",
            "type": "index"
        }
        rowMap["action/zoom"] = {
            "key": "action/zoom",
            "title": "Interface scale (Zoom)",
            "description": "Appearance · Change text and control size",
            "type": "index"
        }
        for (let index = 0; index < SettingsNavigation.categories.length; ++index) {
            const category = SettingsNavigation.categories[index]
            rowMap["action/category/" + category.id] = {
                "key": "action/category/" + category.id,
                "title": category.title,
                "categoryId": category.id,
                "description": "",
                "type": "index"
            }
            rowMap[disclosureKey(category.id)] = {
                "key": disclosureKey(category.id),
                "group": category.title,
                "categoryId": category.id,
                "title": "Advanced",
                "description": "More " + category.title.toLocaleLowerCase() + " settings",
                "type": "submenu"
            }
        }
        for (let index = 0; index < schema.length; ++index) {
            const row = schema[index]
            rowMap[row.key] = row
            sourceRows.push({
                                "rowKey": row.key,
                                "detailLevel": SettingsNavigation.detailLevel(row),
                                "group": row.categoryId,
                                "sourceIndex": index * 2
                            })
            if (row.key === "action/accounts")
                sourceRows.push({
                                    "rowKey": "action/settingsSync",
                                    "detailLevel": 0,
                                    "group": "sources",
                                    "sourceIndex": index * 2 + 1
                                })
        }
        rowsByKey = rowMap
        allSettingsRows = sourceRows
        refreshSettingsFilter(true)
    }

    function reconcileSettingsRows(nextRows, targetKey, takeFocus) {
        const previousY = settingsList.contentY
        const selectionRetained = selectedRowKey === targetKey
        settingsList.autoPositionCurrentItem = false
        reconcilingSettingsRows = true
        SettingsNavigation.reconcileRows(settingsRows, nextRows)
        const target = SettingsNavigation.indexForRowKey(settingsRows, targetKey)
        if (selectedRowKey !== targetKey) {
            finishRowEdit()
            navigationMode = "row"
        }
        currentIndex = target
        selectedRowKey = target >= 0 ? targetKey : ""
        settingsList.currentIndex = target
        settingsList.forceLayout()
        reconcilingSettingsRows = false
        settingsList.autoPositionCurrentItem = true
        if (takeFocus === false && selectionRetained)
            settingsList.contentY = previousY
        else if (target >= 0)
            settingsList.positionViewAtIndex(target, ListView.Contain)
        if (takeFocus !== false)
            InputKeys.focus(settingsList)
        return target
    }

    function refreshSettingsFilter(resetSelection) {
        if (!resetSelection && (modalVisible || editingKey.length))
            return
        const selectedDescriptorIndex = SettingsNavigation.indexForRowKey(settingsRows, selectedRowKey)
        const selectedDescriptor = selectedDescriptorIndex >= 0 ? settingsRows.get(selectedDescriptorIndex) : null
        const selectedSourceIndex = selectedDescriptor ? Number(selectedDescriptor.sourceIndex) : 0
        const selectedRow = rowsByKey[selectedRowKey]
        const nextRows = rebuildVisibleRows()
        let targetKey = resetSelection ? "" : selectedRowKey
        if (SettingsNavigation.indexForRowKey(nextRows, targetKey) < 0 && !resetSelection && selectedRow) {
            const groupDisclosure = disclosureKey(selectedRow.categoryId)
            if (SettingsNavigation.indexForRowKey(nextRows, groupDisclosure) >= 0)
                targetKey = groupDisclosure
        }
        if (SettingsNavigation.indexForRowKey(nextRows, targetKey) < 0)
            targetKey = SettingsNavigation.nearestRowKey(nextRows, selectedSourceIndex)
        if (SettingsNavigation.indexForRowKey(nextRows, targetKey) < 0 && nextRows.length > 0)
            targetKey = nextRows[0].rowKey
        reconcileSettingsRows(nextRows, targetKey, false)
    }

    function currentRow() {
        return rowAtVisibleIndex(currentIndex)
    }

    function rowAtVisibleIndex(index) {
        if (index < 0 || index >= settingsRows.count)
            return null
        return rowsByKey[settingsRows.get(index).rowKey] || null
    }

    function selectRow(index, takeFocus) {
        if (index !== currentIndex) {
            const focused = root.Window.window ? root.Window.window.activeFocusItem : null
            if (editingKey.length && InputKeys.isTextInputItem(focused))
                InputKeys.focus(settingsList)
            finishRowEdit()
            navigationMode = "row"
        }
        const target = SettingsNavigation.clampIndex(index, settingsRows.count)
        if (target < 0) {
            currentIndex = -1
            selectedRowKey = ""
            settingsList.currentIndex = -1
            InputKeys.focus(settingsList)
            return
        }
        const changed = settingsList.currentIndex !== target
        currentIndex = target
        selectedRowKey = settingsRows.get(target).rowKey
        settingsList.currentIndex = target
        if (!changed && settingsList.autoPositionCurrentItem)
            settingsList.positionViewAtIndex(target, ListView.Contain)
        if (takeFocus !== false)
            InputKeys.focus(settingsList)
    }

    function focusEntry() {
        if (modalVisible || editingKey.length || searchField.editing)
            return
        if (settingsRows.count <= 0) {
            selectRow(-1, true)
            return
        }
        const selectedIndex = SettingsNavigation.indexForRowKey(settingsRows, selectedRowKey)
        const previousY = settingsList.contentY
        settingsList.autoPositionCurrentItem = false
        selectRow(selectedIndex >= 0 ? selectedIndex : Math.max(0, currentIndex), false)
        settingsList.autoPositionCurrentItem = true
        InputKeys.focus(settingsList)
        settingsList.contentY = previousY
    }

    function rowControlAt(index) {
        const delegate = settingsList.itemAtIndex(index)
        return delegate ? delegate.controlItem : null
    }

    function settingsValue(row) {
        switch (row.key) {
        case "subtitles/language":
            return Settings.subtitleLanguageIndex
        case "audio/language":
            return Settings.audioLanguageIndex
        default:
            const value = Settings.values[row.key]
            return value === undefined ? row.defaultValue : value
        }
    }

    function rowDescription(row) {
        if (pageMode === "search")
            return SettingsNavigation.categoryTitle(row.categoryId) + " · " + row.group + (availabilityReason(
                                                                                               row).length ? " · "
                                                                                                             + availabilityReason(
                                                                                                                 row) : "")
        const reason = availabilityReason(row)
        if (reason.length)
            return (row.description ? row.description + " · " : "") + reason
        if (row.destination === "subtitleSettings")
            return "Open in Subtitle appearance · " + (row.description || "")
        if (row.destination === "player")
            return "Open in the playback audio sync panel"
        if (row.key === "action/settingsSync")
            return SettingsSync.summary
        if (row.key === "action/connectionSpeed")
            return App.connectionSpeedDescription
        if (row.key === "action/downloads")
            return Downloads.jobs.length ? Downloads.jobs.length + " downloads · Play saved files offline" :
                                           "Manage downloads and play saved files offline"
        if (row.key === "action/downloadDestination" || row.key === "action/mobileDownloadDestination")
            return Downloads.destination + " · Applies only to new downloads"
        if (row.key === "action/accounts") {
            const count = Providers.accounts.length
            return count === 1 ? "1 account" : count + " accounts"
        }
        if (row.key === "action/providers" && Store.updates.length > 0)
            return Store.updates.length === 1 ? "1 update" : Store.updates.length + " updates"
        if (row.key === "subtitles/mode" || row.key === "audio/trackMode") {
            const index = rowCurrentIndex(row)
            const labels = rowOptions(row)
            return index >= 0 && index < labels.length ? labels[index] : row.description
        }
        return row.description || ""
    }

    // Choice labels may carry a "%1" placeholder for the user's preferred
    // language, e.g. "Smart (English when available)".
    function preferredLanguageWord() {
        const labels = Settings.subtitleLanguageOptions
        const index = Settings.subtitleLanguageIndex
        if (index <= 0 || index >= labels.length)
            return "your language"
        return String(labels[index]).split(" ")[0]
    }

    function substitutedLabels(labels) {
        const word = preferredLanguageWord()
        const result = []
        for (let index = 0; index < labels.length; ++index)
            result.push(String(labels[index]).replace("%1", word))
        return result
    }

    function rowValueText(row) {
        if (row.type === "toggle")
            return Boolean(settingsValue(row)) ? "On" : "Off"
        if (row.type === "slider")
            return String(settingsValue(row)) + (row.unitText || "")
        if (row.type === "select") {
            const labels = rowOptions(row)
            const index = rowCurrentIndex(row)
            return index >= 0 && index < labels.length ? labels[index] : String(settingsValue(row) || "")
        }
        if (row.type === "text")
            return String(settingsValue(row) || "")
        if (row.key === "action/settingsSync")
            return "Manage"
        if (row.key === "action/connectionSpeed")
            return "Measure again"
        if (row.key === "action/downloads")
            return "Manage"
        if (row.key === "action/downloadDestination" || row.key === "action/mobileDownloadDestination")
            return "Change…"
        if (row.key === "action/accounts" || row.key === "action/providers")
            return "Manage"
        if (row.key === "action/openSourceNotices" || row.key === "action/exportDiagnostics" || row.key
                === "action/subtitleSettings" || row.key === "action/manageCertificates")
            return "Open"
        if (row.key === "action/clearLatencyStatistics" || row.key === "action/clearLogs")
            return "Clear"

        if (row.key === "about/version")
            return "v" + Qt.application.version
        if (row.key === "about/locale")
            return I18n.currentLocale
        return ""
    }

    function rowOptions(row) {
        if (row.key === "home/providerId")
            return Home.providerChoices.map(function (choice) {
                return choice.name + (choice.version ? " · " + choice.version : "")
            })
        if (row.key === "i18n/locale") {
            const result = []
            for (let index = 0; index < I18n.availableLocales.length; ++index)
                result.push(I18n.displayNameFor(I18n.availableLocales[index]))
            return result
        }
        if (row.key === "subtitles/language" || row.key === "audio/language")
            return Settings.subtitleLanguageOptions
        if (row.key === "subtitles/mode" || row.key === "audio/trackMode")
            return substitutedLabels(row.choiceLabels || [])
        return row.choiceLabels || []
    }

    function rowChoiceValues(row) {
        if (row.key === "home/providerId")
            return Home.providerChoices.map(function (choice) {
                return choice.id
            })
        if (row.key === "i18n/locale")
            return I18n.availableLocales
        if (row.key === "subtitles/language" || row.key === "audio/language")
            return Settings.subtitleLanguageOptions
        return row.choiceValues || []
    }

    function valueIndex(values, value) {
        for (let index = 0; index < values.length; ++index)
            if (String(values[index]) === String(value))
                return index
        return 0
    }

    function rowCurrentIndex(row) {
        if (row.key === "subtitles/language")
            return Settings.subtitleLanguageIndex
        if (row.key === "audio/language")
            return Settings.audioLanguageIndex
        return valueIndex(rowChoiceValues(row), settingsValue(row))
    }

    function setRowValue(row, value, index) {
        switch (row.key) {
        case "subtitles/language":
            Settings.setSubtitleLanguageIndex(index)
            break
        case "audio/language":
            Settings.setAudioLanguageIndex(index)
            break
        default:
            Settings.setValue(row.key, value)
        }
    }

    function setRowChoice(row, index) {
        const values = rowChoiceValues(row)
        if (index < 0 || index >= values.length)
            return
        if (row.key === "playback/mpvConfigMode" && values[index] === "custom" && !String(
                    Settings.values["playback/mpvConfigDirectory"] || "").length && !Platform.isTV) {
            pendingCustomMpvMode = true
            openMpvFolderDialog()
            return
        }
        setRowValue(row, values[index], index)
    }

    function toggleAdvancedGroup(group, index) {
        const key = disclosureKey(group)
        const disclosureIndex = SettingsNavigation.indexForRowKey(settingsRows, key)
        if (disclosureIndex < 0)
            return
        settingsList.autoPositionCurrentItem = false
        reconcilingSettingsRows = true
        currentIndex = disclosureIndex
        selectedRowKey = key
        settingsList.currentIndex = disclosureIndex
        InputKeys.focus(settingsList)

        const next = Object.assign({}, expandedGroups)
        next[group] = !Boolean(next[group])
        expandedGroups = next
        SettingsNavigation.reconcileRows(settingsRows, rebuildVisibleRows())
        const settledIndex = SettingsNavigation.indexForRowKey(settingsRows, key)
        currentIndex = settledIndex
        selectedRowKey = settledIndex >= 0 ? key : ""
        settingsList.currentIndex = settledIndex
        settingsList.forceLayout()
        reconcilingSettingsRows = false
        settingsList.autoPositionCurrentItem = true
        if (settledIndex >= 0)
            settingsList.positionViewAtIndex(settledIndex, ListView.Contain)
    }

    function activateRow(row, index) {
        if (!row)
            return
        selectRow(index, false)
        if (row.type === "index") {
            if (row.key === "action/searchSettings")
                openSearch()
            else if (row.key === "action/zoom")
                openCategory("appearance", "appearance/uiScalePercent")
            else
                openCategory(row.categoryId, "")
            return
        }
        if (pageMode === "search") {
            openSearchResult(row)
            return
        }
        if (availabilityReason(row).length) {
            if (row.dependsOnKey) {
                const prerequisite = rowsByKey[row.dependsOnKey]
                if (prerequisite)
                    openCategory(prerequisite.categoryId, prerequisite.key)
            }
            return
        }
        if (row.destination === "subtitleSettings" && shell) {
            shell.pushRoute("subtitleSettings", {
                                "rowKey": row.destinationKey,
                                "advanced": SettingsNavigation.detailLevel(row) > 0
                            })
            return
        }
        if (row.destination === "player" && shell) {
            shell.openPlaybackSetting(row.destinationKey)
            return
        }
        if (row.type === "submenu") {
            toggleAdvancedGroup(row.categoryId, index)
            return
        }
        if (row.type === "action") {
            if (row.key === "action/settingsSync" && shell)
                shell.pushRoute("settingsSync")
            else if (row.key === "action/accounts" && shell)
                shell.pushRoute("accounts")
            else if (row.key === "action/providers" && shell)
                shell.pushRoute("addProvider")
            else if (row.key === "action/downloads" && shell)
                shell.openDownloads("", settingsList)
            else if ((row.key === "action/downloadDestination" || row.key === "action/mobileDownloadDestination")
                     && shell)

                shell.openDownloads("", settingsList, true)
            else if (row.key === "action/manageCertificates")
                certificateManagerVisible = true
            else if (row.key === "action/clearLatencyStatistics")
                InputLatency.clearStatistics()
            else if (row.key === "action/clearLogs")
                App.clearLogs()
            else if (row.key === "action/connectionSpeed")
                App.refreshConnectionSpeed()
            else if (row.key === "action/exportDiagnostics") {
                diagnosticsExportPreview = App.diagnosticsPreview()
                diagnosticsExportVisible = true
            } else if (row.key === "action/subtitleSettings" && shell)
                shell.pushRoute("subtitleSettings")
            else if (row.key === "action/openSourceNotices" && shell)
                shell.pushRoute("openSourceNotices")
        } else if (row.type === "toggle") {
            setRowValue(row, !Boolean(settingsValue(row)), -1)
        } else if (row.type === "select") {
            if (!rowOptions(row).length)
                return
            beginRowEdit(row)
            settingsList.positionViewAtIndex(index, ListView.Contain)
            Qt.callLater(function () {
                const anchor = rowControlAt(index)
                if (!anchor)
                    return
                navigationMode = "row"
                choiceDialogRow = row
                choiceDialogAnchor = anchor
                choiceDialogVisible = true
            })
        } else if (row.type === "text") {
            const control = rowControlAt(index)
            if (control && control.activate)
                control.activate()
        }
    }

    function adjustRow(row, direction) {
        if (!row)
            return false
        if (row.type === "select") {
            const control = rowControlAt(settingsList.currentIndex)
            if (control && control.move)
                return control.move(direction)
            return true
        }
        if (row.type === "slider") {
            const from = Number(row.from || 0)
            const to = Number(row.to || 100)
            const current = Number(settingsValue(row))
            const next = row.key === "playback/forwardCacheSizeMiB" ? current * (direction > 0 ? 2 : 0.5) : current
                                                                      + Number(row.step || 1) * direction
            setRowValue(row, Math.max(from, Math.min(to, next)), -1)
            return true
        }
        if (row.type === "text") {
            const control = rowControlAt(settingsList.currentIndex)
            return control && control.move ? control.move(direction) : true
        }
        return false
    }

    function closeChoiceDialog() {
        choiceDialogVisible = false
        finishRowEdit()
        choiceDialogAnchor = null
        choiceDialogRow = null
        Qt.callLater(function () {
            selectRow(currentIndex, true)
        })
    }

    function makeChoiceSpace(pixels) {
        const maximum = Math.max(0, settingsList.contentHeight + settingsList.bottomMargin - settingsList.height)
        settingsList.contentY = Math.min(maximum, Math.max(0, settingsList.contentY + pixels))
        if (choiceDialog)
            Qt.callLater(choiceDialog.completePresentation)
    }

    function back() {
        if (diagnosticsExportVisible) {
            diagnosticsExportLoader.item.back()
            return true
        }
        if (certificateManagerVisible) {
            certificateManagerLoader.item.back()
            return true
        }
        if (choiceDialogVisible) {
            closeChoiceDialog()
            return true
        }
        if (mpvFolderDialogVisible)
            return true
        if (navigationMode !== "row")
            return routeAction("back")
        if (searchField.editing) {
            Qt.inputMethod.hide()
            InputKeys.focus(settingsList)
            return true
        }
        if (inputCompositionActive())
            return true
        if (searchResultLocation) {
            const saved = searchResultLocation
            searchResultLocation = null
            restoreLocation(saved)
            return true
        }
        if (pageMode === "search") {
            const saved = priorSearchLocation
            priorSearchLocation = null
            restoreLocation(saved)
            return true
        }
        if (pageMode === "category") {
            categoryLocations[categoryId] = location()
            const previousCategory = categoryId
            pageMode = "index"
            revealedRowKey = ""
            reconcileSettingsRows(rebuildVisibleRows(), "action/category/" + previousCategory, true)
            return true
        }
        return false
    }

    function routeAction(action, key, phase, repeat) {
        const focused = root.Window.window ? root.Window.window.activeFocusItem : null
        if (inputCompositionActive())
            return false
        if (searchButton.activeFocus || backButton.activeFocus) {
            if (action === "activate")
                return searchButton.activeFocus ? openSearch() : back()
            if (action === "down") {
                InputKeys.focus(settingsList)
                return true
            }
            if (action === "left" && backButton.visible) {
                InputKeys.focus(backButton)
                return true
            }
            if (action === "right") {
                InputKeys.focus(searchButton)
                return true
            }
            if (action === "up" && shell) {
                shell.focusNavBar()
                return true
            }
            return false
        }
        if (pageMode === "search" && (searchField.activeFocus || searchField.editing)) {
            if (action === "down" || action === "activate") {
                Qt.inputMethod.hide()
                InputKeys.focus(settingsList)
                return true
            }
            return false
        }
        if (InputKeys.isTextInputItem(focused) && navigationMode !== "value-editing")
            return false
        if (navigationMode === "row") {
            if (key !== undefined && SettingsNavigation.consumeRecoveryGesture(settingsList, root, InputKeys,
                                                                               recoveryGesture, key, phase, repeat))
                return true
            if (action === "activate" && SettingsNavigation.recoverVisibleSelection(settingsList, root, InputKeys))
                return true
        }
        const row = currentRow()
        const previousMode = navigationMode
        const route = SettingsNavigation.valueRoute(navigationMode, action, Boolean(pageMode === "category" && row && !availabilityReason(
                                                                                        row).length && (
                                                                                        !row.destination
                                                                                        || row.destination
                                                                                        === "category") && (row.type
                                                                                                            === "slider"
                                                                                                            || row.type
                                                                                                            === "text")))
        navigationMode = route.mode
        if (previousMode === "value-editing" && route.mode !== "value-editing") {
            const control = rowControlAt(currentIndex)
            if (row && row.type === "text" && control && control.finishEditing)
                control.finishEditing(action === "activate")
            // Numeric fields commit on blur. End the sync edit only after that
            // commit, so a deferred remote value cannot replace typed intent.
            InputKeys.focus(settingsList)
            finishRowEdit()
        }
        switch (route.effect) {
        case "begin-edit":
            beginRowEdit(row)
            if (row && row.type === "text") {
                const control = rowControlAt(currentIndex)
                if (control && control.activate)
                    control.activate()
            }
            return true
        case "end-edit":
            return true
        case "activate":
            activateRow(row, currentIndex)
            return true
        case "value":
            // A focused text field owns its caret; the page must not turn a
            // horizontal key into a sync action or another focus target.
            const focused = root.Window.window ? root.Window.window.activeFocusItem : null
            if (previousMode === "value-editing" && (InputKeys.isTextInputItem(focused) || (row && row.type
                                                                                            === "text")))
                return false
            if (pageMode !== "category" || !row || availabilityReason(row).length || (row.destination
                                                                                      && row.destination
                                                                                      !== "category"))
                return true
            return adjustRow(row, action === "right" ? 1 : -1)
        case "move-up":
        case "move-down":
            InputKeys.focus(settingsList)
            return settingsList.moveSelection(route.effect === "move-up" ? -1 : 1)
        case "back":
            return false
        default:
            return true
        }
    }

    function routeKey(key, phase, repeat) {
        if (phase === "release" && InputKeys.isDirection(key))
            SettingsNavigation.consumeRecoveryGesture(settingsList, root, InputKeys, recoveryGesture, key, phase,
                                                      repeat)
        if (certificateManagerVisible)
            return certificateManagerLoader.item.routeKey(key, phase, repeat)
        if (diagnosticsExportVisible)
            return diagnosticsExportLoader.item.routeKey(key, phase, repeat)
        if (mpvFolderDialogVisible)
            return true
        if (choiceDialogVisible)
            return choiceDialog ? choiceDialog.routeKey(key, phase, repeat) : true
        if (inputCompositionActive())
            return false
        if (phase === "release" && InputKeys.isDirection(key))
            return true
        if (key === Qt.Key_Right)
            return routeAction("right", key, phase, repeat)
        if (key === Qt.Key_Left)
            return routeAction("left", key, phase, repeat)
        if (key === Qt.Key_Up)
            return routeAction("up", key, phase, repeat)
        if (key === Qt.Key_Down)
            return routeAction("down", key, phase, repeat)
        return false
    }

    function activate() {
        if (diagnosticsExportVisible) {
            diagnosticsExportLoader.item.activate()
            return
        }
        if (mpvFolderDialogVisible)
            return
        if (certificateManagerVisible) {
            certificateManagerLoader.item.activate()
            return
        }
        if (choiceDialogVisible)
            choiceDialog.activate()
        else
            routeAction("activate")
    }

    focus: true
    onActiveFocusChanged: if (activeFocus)
                              focusEntry()
    onVisibleChanged: {
        if (visible) {
            ensureRowsBuilt()
            SettingsSync.refresh()
        } else {
            recoveryGesture.key = 0
            finishRowEdit()
            navigationMode = "row"
        }
        if (visible && activeFocus)
            Qt.callLater(focusEntry)
    }

    property bool rowsBuilt: false

    function ensureRowsBuilt() {
        if (rowsBuilt)
            return
        rowsBuilt = true
        buildSettingsRowsSource()
    }

    // Only take focus if the page is actually active: the route host
    // prewarms an invisible instance, which must not steal focus.
    Component.onCompleted: Qt.callLater(function () {
        ensureRowsBuilt()
        if (visible)
            SettingsSync.refresh()
        if (activeFocus)
            focusEntry()
    })
    // The same primitive the library grid uses to decide when its view has
    // actually put something on screen. It drives itself: rows appearing
    // flips `enabled`, which schedules the check, which keeps retrying until
    // the first delegate exists. Only the first one is looked at -- that is
    // what readiness turns on, and walking the rest would cost more than it
    // could tell us.
    AtomicViewReveal {
        id: settingsReveal

        view: settingsList
        latencyMonitor: InputLatency
        transitionToken: root.uiTransitionToken
        enabled: settingsRows.count > 0
        firstIndex: 0
        lastIndex: 0
    }

    Connections {
        target: Settings

        function onSettingChanged(key) {
            root.refreshSettingsFilter(false)
        }
    }
    Connections {
        target: ProviderCapabilities
        function onChanged() {
            root.refreshSettingsFilter(false)
        }
    }
    Connections {
        target: Downloads
        function onEnabledChanged() {
            root.refreshSettingsFilter(false)
        }
    }
    Connections {
        target: Player
        function onHdrPlaybackChanged() {
            root.refreshSettingsFilter(false)
        }
    }

    Column {
        id: settingsHeader
        anchors.top: parent.top
        anchors.topMargin: Metrics.pageMarginPx
        anchors.horizontalCenter: parent.horizontalCenter
        width: settingsList.width
        spacing: Metrics.scaled(10)
        AppText {
            width: parent.width
            text: root.pageTitle
            font.pixelSize: Metrics.titleSizePx
            wrapMode: Text.Wrap
            Accessible.name: text
            Accessible.role: Accessible.Heading
        }
        Row {
            width: parent.width
            spacing: Metrics.scaled(12)
            ActionButton {
                id: backButton
                text: "Back"
                visible: root.pageMode !== "index"
                width: Math.min(Metrics.scaled(100), Math.max(0, (parent.width - parent.spacing) / 2))
                onClicked: root.back()
            }
            ActionButton {
                id: searchButton
                text: "Search"
                width: Math.min(Metrics.scaled(120), Math.max(0, (parent.width - parent.spacing) / 2))
                onClicked: root.openSearch()
            }
        }
        TextFieldRow {
            id: searchField
            width: parent.width
            visible: root.pageMode === "search"
            label: "Search settings"
            placeholderText: "Name, value or keyword (for example zoom)"
            text: root.searchQuery
            onTextEdited: text => {
                root.searchQuery = text
                root.refreshSettingsFilter(true)
            }
            onAccepted: {
                Qt.inputMethod.hide()
                InputKeys.focus(settingsList)
            }
        }
        SecondaryText {
            visible: root.pageMode === "search"
            width: parent.width
            text: settingsRows.count ? settingsRows.count + " results · Open a result to see it in context" :
                                       "No matching settings"
            wrapMode: Text.Wrap
            Accessible.name: text
        }
    }

    MenuListView {
        id: settingsList
        readonly property real pageInset: Metrics.pageMarginPx
        width: Math.min(Math.max(0, parent.width - pageInset * 2), Metrics.scaled(1280))
        anchors.top: settingsHeader.bottom
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: Metrics.scaled(12)
        anchors.bottomMargin: pageInset
        bottomMargin: root.choiceDialogVisible && root.choiceDialog ? root.choiceDialog.panelHeight + Metrics.scaled(16) :
                                                                      0
        model: settingsRows
        dismissOnBack: false
        dismissOnHorizontal: false
        spacing: Metrics.scaled(10)
        // Settings rows are costly composites. Do not construct an extra
        // viewport of hidden controls while opening the page.
        cacheBuffer: 0
        onCurrentIndexChanged: {
            if (root.reconcilingSettingsRows)
                return
            if (root.currentIndex !== currentIndex) {
                root.finishRowEdit()
                root.navigationMode = "row"
            }
            root.currentIndex = currentIndex
            root.selectedRowKey = currentIndex >= 0 && currentIndex < settingsRows.count ? settingsRows.get(
                                                                                               currentIndex).rowKey : ""
        }
        onAccepted: index => root.routeAction("activate")
        onEdgeUp: {
            if (root.pageMode === "search")
                searchField.focusRow()
            else
                InputKeys.focus(searchButton)
        }
        delegate: Column {
            id: settingsDelegate
            required property int index
            required property string rowKey
            required property bool showHeader
            required property bool advanced
            required property real sourceIndex
            readonly property var rowData: root.rowsByKey[rowKey]
            // The view marks exactly one delegate as current, so the highlight
            // cannot land on two rows at once. Comparing a per-row copy of the
            // index against currentIndex could: rows the model inserts or
            // removes shift every delegate below them, and any copy taken
            // before the shift then matches a row it no longer belongs to.
            readonly property bool rowCurrent: ListView.isCurrentItem && settingsList.activeFocus
            width: settingsList.width
            Component.onCompleted: InputLatency.noteDelegate("settings_row", 1)
            Component.onDestruction: InputLatency.noteDelegate("settings_row", -1)
            readonly property var controlItem: rowLoader.item
            spacing: Metrics.scaled(10)

            GroupHeader {
                width: parent.width
                visible: parent.showHeader
                title: SettingsNavigation.categoryTitle(root.categoryId)
            }
            Item {
                width: Math.max(0, parent.width - (settingsDelegate.advanced ? Metrics.scaled(24) : 0))
                x: settingsDelegate.advanced ? Metrics.scaled(24) : 0
                height: rowLoader.height
                Loader {
                    id: rowLoader
                    width: parent.width
                    readonly property var row: settingsDelegate.rowData
                    readonly property int rowIndex: settingsDelegate.index
                    readonly property bool rowCurrent: settingsDelegate.rowCurrent
                    sourceComponent: root.pageMode !== "category" || root.availabilityReason(row).length || (row.destination
                                                                                                             && row.destination
                                                                                                             !== "category")
                                     ? settingComponent : row.type === "toggle" ? toggleComponent : row.type
                                                                                  === "select" ? selectComponent :
                                                                                                 row.type === "slider"
                                                                                                 ? sliderComponent :
                                                                                                   row.type === "text"
                                                                                                   ? textComponent :
                                                                                                     settingComponent
                }
            }
        }
    }

    Component {
        id: settingComponent
        SettingRow {
            id: settingRow
            readonly property var row: parent ? parent.row : null
            readonly property int rowIndex: parent ? parent.rowIndex : -1
            readonly property bool isSubmenu: row && row.type === "submenu"
            width: parent ? parent.width : settingsList.width
            focus: false
            focusPolicy: Qt.NoFocus
            rowFocus: parent ? parent.rowCurrent : false
            title: row ? row.title : ""
            description: row ? root.rowDescription(row) : ""
            valueText: row ? root.rowValueText(row) : ""
            valueTextVisible: !isSubmenu
            pointerActivationEnabled: row && (root.pageMode !== "category" || row.type === "action" || row.type
                                              === "index" || isSubmenu || row.dependsOnKey || (row.destination
                                                                                               && row.destination
                                                                                               !== "category"))
            trailing: [
                MaterialIcon {
                    visible: settingRow.isSubmenu
                    name: root.groupExpanded(settingRow.row ? settingRow.row.categoryId : "") ? "expand_less" :
                                                                                                "expand_more"
                    iconSize: Math.max(20, Metrics.iconSizePx)
                    iconColor: Theme.textSecondary
                }
            ]
            onClicked: {
                root.selectRow(rowIndex, true)
                root.activateRow(row, rowIndex)
            }
        }
    }

    Component {
        id: toggleComponent
        ToggleRow {
            readonly property var row: parent ? parent.row : null
            readonly property int rowIndex: parent ? parent.rowIndex : -1
            width: parent ? parent.width : settingsList.width
            focus: false
            focusPolicy: Qt.NoFocus
            rowFocus: parent ? parent.rowCurrent : false
            title: row ? row.title : ""
            description: row ? root.rowDescription(row) : ""
            checked: row ? Boolean(root.settingsValue(row)) : false
            onToggled: checked => {
                root.selectRow(rowIndex, true)
                root.navigationMode = "row"
                root.setRowValue(row, checked, -1)
            }
        }
    }

    Component {
        id: selectComponent
        SelectRow {
            readonly property var row: parent ? parent.row : null
            readonly property int rowIndex: parent ? parent.rowIndex : -1
            width: parent ? parent.width : settingsList.width
            focus: false
            focusPolicy: Qt.NoFocus
            rowFocus: parent ? parent.rowCurrent : false
            title: row ? row.title : ""
            description: row ? root.rowDescription(row) : ""
            onOpened: {
                root.selectRow(rowIndex, true)
                root.navigationMode = "row"
                root.activateRow(row, rowIndex)
            }
            options: row ? root.rowOptions(row) : []
            currentIndex: row ? root.rowCurrentIndex(row) : 0
            onSelected: (index, value) => root.setRowChoice(row, index)
        }
    }

    Component {
        id: sliderComponent
        SliderRow {
            readonly property var row: parent ? parent.row : null
            readonly property int rowIndex: parent ? parent.rowIndex : -1
            width: parent ? parent.width : settingsList.width
            selected: parent ? parent.rowCurrent : false
            title: row ? row.title : ""
            description: row ? root.rowDescription(row) : ""
            from: row ? Number(row.from) : 0
            to: row ? Number(row.to) : 100
            step: row ? Number(row.step || 1) : 1
            logarithmic: Boolean(row && row.key === "playback/forwardCacheSizeMiB")
            unitText: row ? String(row.unitText || "") : ""
            value: row ? Number(root.settingsValue(row)) : 0
            onValueEdited: value => {
                root.setRowValue(row, value, -1)
                root.finishRowEdit()
                root.navigationMode = "row"
                InputKeys.focus(settingsList)
            }
            onInteractionStarted: {
                root.selectRow(rowIndex, false)
                root.beginRowEdit(row)
                root.navigationMode = "value-editing"
            }
        }
    }
    Component {
        id: textComponent

        Surface {
            id: textRow
            readonly property var row: parent ? parent.row : null
            readonly property int rowIndex: parent ? parent.rowIndex : -1
            width: parent ? parent.width : settingsList.width
            implicitHeight: textContent.implicitHeight + Metrics.scaled(28)
            elevated: true
            focused: parent ? parent.rowCurrent : false

            function activate() {
                if (browseButton.visible && browseButton.activeFocus)
                    root.openMpvFolderDialog()
                else
                    pathField.focusField()
            }

            function move(direction) {
                if (direction > 0 && browseButton.visible)
                    InputKeys.focus(browseButton)
                else
                    pathField.focusField()
                return true
            }
            function finishEditing(commitValue) {
                if (commitValue)
                    root.setRowValue(row, pathField.text, -1)
                else
                    pathField.text = String(root.settingsValue(row) || "")
                Qt.inputMethod.hide()
            }

            Column {
                id: textContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: Metrics.scaled(18)
                spacing: Metrics.scaled(8)

                AppText {
                    width: parent.width
                    text: row ? row.title : ""
                    color: Theme.textPrimary
                    font.pixelSize: Metrics.bodySizePx
                    font.weight: Font.DemiBold
                }

                AppText {
                    width: parent.width
                    text: row ? root.rowDescription(row) : ""
                    color: Theme.textSecondary
                    font.pixelSize: Metrics.metaSizePx
                    wrapMode: Text.Wrap
                }

                Row {
                    width: parent.width
                    spacing: Metrics.scaled(10)

                    TextFieldRow {
                        id: pathField
                        width: browseButton.visible ? Math.max(0, parent.width - browseButton.width - parent.spacing) :
                                                      parent.width
                        label: "Directory"
                        text: row ? String(root.settingsValue(row) || "") : ""
                        placeholderText: "/absolute/path/to/mpv"
                        inputMethodHints: Qt.ImhNoPredictiveText
                        onEditingChanged: {
                            if (editing) {
                                root.selectRow(rowIndex, false)
                                root.beginRowEdit(row)
                                root.navigationMode = "value-editing"
                            }
                        }
                        onAccepted: {
                            textRow.finishEditing(true)
                            root.finishRowEdit()
                            root.navigationMode = "row"
                            Qt.callLater(function () {
                                root.selectRow(rowIndex, true)
                            })
                        }
                    }

                    ActionButton {
                        id: browseButton
                        visible: !Platform.isTV
                        width: visible ? Metrics.scaled(132) : 0
                        height: pathField.height
                        text: "Browse"
                        iconName: "folder"
                        onClicked: {
                            root.selectRow(textRow.rowIndex, false)
                            root.beginRowEdit(textRow.row)
                            root.openMpvFolderDialog()
                        }
                    }
                }
            }
        }
    }

    function openMpvFolderDialog() {
        if (Platform.isTV)
            return
        mpvFolderDialogVisible = true
        if (mpvFolderDialog)
            mpvFolderDialog.open()
        else
            mpvFolderDialogLoader.active = true
    }

    Loader {
        id: mpvFolderDialogLoader
        active: false
        source: active ? Qt.resolvedUrl("DesktopFolderDialog.qml") : ""
        onLoaded: item.open()
    }

    Connections {
        target: root.mpvFolderDialog

        function onFolderSelected(folder) {
            Settings.setValue("playback/mpvConfigDirectory", folder)
            if (root.pendingCustomMpvMode)
                Settings.setValue("playback/mpvConfigMode", "custom")
            root.pendingCustomMpvMode = false
            root.mpvFolderDialogVisible = false
            root.finishRowEdit()
            Qt.callLater(root.focusEntry)
        }

        function onDismissed() {
            root.pendingCustomMpvMode = false
            root.mpvFolderDialogVisible = false
            root.finishRowEdit()
            Qt.callLater(root.focusEntry)
        }
    }

    Loader {
        id: diagnosticsExportLoader
        anchors.fill: parent
        active: root.diagnosticsExportVisible
        z: 200
        sourceComponent: ConfirmationDialog {
            title: Platform.isAndroid ? "Share diagnostics?" : "Save diagnostics report?"
            message: root.diagnosticsExportPreview
            confirmText: Platform.isAndroid ? "Share" : "Save"
            onAccepted: {
                App.saveDiagnosticsReport()
                root.diagnosticsExportVisible = false
                InputKeys.focus(settingsList)
            }
            onDismissed: {
                root.diagnosticsExportVisible = false
                InputKeys.focus(settingsList)
            }
        }
    }

    Loader {
        id: certificateManagerLoader
        anchors.fill: parent
        active: root.certificateManagerVisible
        z: 200
        sourceComponent: RememberedCertificatesDialog {
            trustController: TlsTrust
            inputKeys: InputKeys
            onDismissed: {
                root.certificateManagerVisible = false
                InputKeys.focus(settingsList)
            }
        }
    }

    Loader {
        id: choiceDialogLoader
        anchors.fill: parent
        active: root.choiceDialogVisible
        sourceComponent: OptionPickerDialog {
            visible: true
            anchorItem: root.choiceDialogAnchor
            title: root.choiceDialogRow ? root.choiceDialogRow.title : "Choose an option"
            options: root.choiceDialogRow ? root.rowOptions(root.choiceDialogRow) : []
            currentIndex: root.choiceDialogRow ? root.rowCurrentIndex(root.choiceDialogRow) : 0
            onSelected: index => {
                if (root.choiceDialogRow)
                    root.setRowChoice(root.choiceDialogRow, index)
                root.closeChoiceDialog()
            }
            onDismissed: root.closeChoiceDialog()
            onSpaceBelowRequired: pixels => root.makeChoiceSpace(pixels)
        }
    }
}
