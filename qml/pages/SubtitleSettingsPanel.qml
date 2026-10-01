pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import "../theme"
import "../primitives"
import "../shell"
import "SettingsNavigation.js" as SettingsNavigation

FocusScope {
    id: root

    property var shell
    property var uiTransitionToken: 0
    property var settingsController: Settings
    property var syncController: typeof SettingsSync !== "undefined" ? SettingsSync : null
    property string syncSourceLabel: {
        if (!syncController)
            return ""
        const accounts = syncController.accounts || []
        const account = accounts.find(candidate => candidate.id === syncController.accountId)
        return account ? String(account.syncLabel || "") : ""
    }
    property var platformInfo: Platform
    property bool hdrPlayback: Player.hdrPlayback
    property string navigationMode: "row"
    property string editingKey: ""
    property bool editingChanged: false
    property bool pointerEditing: false
    readonly property int currentRowIndex: list.currentIndex
    readonly property var reachableSettingKeys: SettingsNavigation.subtitleReachableKeys(
                                                    settingsController.settingsSchema, platformInfo, hdrPlayback, name
                                                    => settingsController.values[name])
    // Over playback the real subtitles are the preview, so the panel steps
    // aside as a sheet instead of drawing an imitation of a film still.
    property bool overVideo: false

    signal dismissed

    property var rows: []
    property bool choiceVisible: false
    property var choiceRow: null
    property Item choiceAnchor: null
    property bool resetVisible: false
    property bool advancedExpanded: false
    property int pendingFocusIndex: -1
    property var appearanceSnapshot: ({})

    readonly property var sections: SettingsNavigation.subtitleSections
    readonly property var advancedSections: SettingsNavigation.subtitleAdvancedSections

    readonly property var appearanceKeys: ["subtitles/styling", "subtitles/textWeight", "subtitles/font",
        "subtitles/textColor", "subtitles/overrideTextColor", "subtitles/dropShadow", "subtitles/textBackground",
        "subtitles/recolorImageSubtitles", "subtitles/bitmapSharpnessPercent", "subtitles/bitmapShadowEnabled",
        "subtitles/bitmapShadowCoreSize", "subtitles/bitmapShadowCoreGrow", "subtitles/bitmapShadowCoreOpacityPercent",
        "subtitles/bitmapShadowSpreadEnabled", "subtitles/bitmapShadowSpreadSize", "subtitles/bitmapShadowSpreadGrow",
        "subtitles/bitmapShadowSpreadX", "subtitles/bitmapShadowSpreadY", "subtitles/bitmapShadowSpreadOpacityPercent",
        "subtitles/bitmapShadowDither", "subtitles/verticalPositionPercent", "subtitles/scalePercent",
        "subtitles/alwaysOverridePositionAndSize", "subtitles/allowInBlackBars", "subtitles/hdrBrightnessPercent"]

    function specValue(spec) {
        if (spec.key === "subtitles/language")
            return settingsController.subtitleLanguageIndex
        const value = settingsController.values[spec.key]
        return value === undefined || value === null ? spec.defaultValue : value
    }

    // Desktop can offer whatever fonts are installed on top of the bundled ones.
    function expandedSpec(spec) {
        if (!spec || spec.key !== "subtitles/font" || !platformInfo.hasSystemFonts)
            return spec
        const expanded = Object.assign({}, spec)
        expanded.choiceLabels = spec.choiceLabels.slice()
        expanded.choiceValues = spec.choiceValues.slice()
        const families = settingsController.systemSubtitleFonts
        for (let index = 0; index < families.length; ++index) {
            expanded.choiceLabels.push("System — " + families[index])
            expanded.choiceValues.push("system:" + families[index])
        }
        return expanded
    }

    // `followAdvanced` keeps the selection with the section as it toggles: on
    // its first row when it opens, and back on the Advanced row itself when it
    // closes, so a list that just lost rows cannot strand the selection at the
    // top of the page.
    function rebuildRows(followAdvanced) {
        const focusedEntry = list.entryAt(list.currentIndex)
        const focusedKey = focusedEntry ? focusedEntry.rowKey : ""
        const schema = settingsController.settingsSchema
        const byKey = {}
        for (let index = 0; index < schema.length; ++index)
            byKey[schema[index].key] = schema[index]

        const resolve = function (key) {
            const spec = byKey[key]
            const available = SettingsNavigation.rowAvailable(spec, platformInfo, hdrPlayback, function (name) {
                const value = root.settingsController.values[name]
                return value === undefined ? "" : value
            })
            return available ? root.expandedSpec(spec) : null
        }
        const visibleRows = SettingsNavigation.sectionedRows(sections, resolve)
        const advancedIndex = visibleRows.length
        visibleRows.push({
                             "section": false,
                             "spec": {
                                 "key": "action/toggleAdvanced",
                                 "title": "Advanced",
                                 "description": "Font, outline, image subtitles, and HDR",
                                 "type": "submenu"
                             }
                         })
        if (advancedExpanded) {
            const advancedRows = SettingsNavigation.sectionedRows(advancedSections, resolve)
            if (followAdvanced) {
                const relativeIndex = SettingsNavigation.firstActionableRow(advancedRows, 0)
                if (relativeIndex >= 0)
                    pendingFocusIndex = visibleRows.length + relativeIndex
            }
            for (let index = 0; index < advancedRows.length; ++index)
                visibleRows.push(advancedRows[index])
        } else if (followAdvanced) {
            pendingFocusIndex = advancedIndex
        }
        for (let index = 0; index < visibleRows.length; ++index) {
            const entry = visibleRows[index]
            entry.rowKey = entry.section ? "section/" + entry.spec.title : entry.spec.key
        }
        rows = visibleRows
        SettingsNavigation.reconcileRows(rowsModel, visibleRows)
        if (!followAdvanced) {
            const retainedIndex = SettingsNavigation.indexForRowKey(rowsModel, focusedKey)
            if (retainedIndex >= 0)
                list.currentIndex = retainedIndex
            else
                list.clampEnabled()
        }
    }

    function choiceLabels(spec) {
        if (spec.key === "subtitles/language")
            return settingsController.subtitleLanguageOptions
        // Some labels carry a "%1" placeholder for the preferred language name.
        if (spec.key === "subtitles/mode") {
            const options = settingsController.subtitleLanguageOptions
            const index = settingsController.subtitleLanguageIndex
            const word = index > 0 && index < options.length ? String(options[index]).split(" ")[0] : "your language"
            const result = []
            for (let i = 0; i < spec.choiceLabels.length; ++i)
                result.push(String(spec.choiceLabels[i]).replace("%1", word))
            return result
        }
        return spec.choiceLabels || []
    }

    function choiceValues(spec) {
        return spec.key === "subtitles/language" ? settingsController.subtitleLanguageOptions : (spec.choiceValues
                                                                                                 || [])

    }

    function currentChoice(spec) {
        if (spec.key === "subtitles/language")
            return settingsController.subtitleLanguageIndex
        const values = choiceValues(spec)
        const value = String(specValue(spec))
        for (let index = 0; index < values.length; ++index)
            if (String(values[index]) === value)
                return index
        return 0
    }

    function setValue(spec, value, index) {
        const before = specValue(spec)
        if (spec.key === "subtitles/language")
            settingsController.setSubtitleLanguageIndex(index)
        else
            settingsController.setValue(spec.key, value)
        if (editingKey === spec.key && before !== (spec.key === "subtitles/language" ? index : value))
            editingChanged = true
    }

    function setChoice(spec, index) {
        const values = choiceValues(spec)
        if (index >= 0 && index < values.length)
            setValue(spec, values[index], index)
    }

    function rowControlAt(index) {
        const delegate = list.itemAtIndex(index)
        return delegate ? delegate.control : null
    }

    function syncState(spec) {
        return syncController && spec ? syncController.states[spec.key] || null : null
    }

    function hasSync(spec) {
        const state = syncState(spec)
        return state !== null && state.eligible === true
    }

    function toggleSync(spec) {
        if (hasSync(spec))
            syncController.setSettingEnabled(spec.key, !syncState(spec).enabled)
    }

    function beginEdit(spec, pointer) {
        if (editingKey === spec.key)
            return
        finishEdit()
        editingKey = spec.key
        editingChanged = false
        pointerEditing = pointer === true
        navigationMode = "value-editing"
        if (syncController)
            syncController.beginEdit(spec.key)
    }

    function finishEdit() {
        const key = editingKey
        const changed = editingChanged
        if (key && pointerEditing && !changed)
            settingsController.previewValue(key, settingsController.values[key])
        editingKey = ""
        editingChanged = false
        pointerEditing = false
        navigationMode = "row"
        if (key && syncController)
            syncController.endEdit(key, changed)
    }

    function routeAction(action) {
        const focused = root.Window.window ? root.Window.window.activeFocusItem : null
        if (pointerEditing && InputKeys.isTextInputItem(focused)) {
            if (action === "left" || action === "right")
                return false
            // The numeric field's existing editingFinished handler commits
            // before releasing the sync edit lock.
            focused.focus = false
            finishEdit()
            InputKeys.focus(list)
            if (action === "activate" || action === "back")
                return true
        }
        const entry = list.entryAt(list.currentIndex)
        const spec = entry && !entry.section ? entry.spec : null
        const previousMode = navigationMode
        const result = SettingsNavigation.syncRoute(navigationMode, action, hasSync(spec), spec && (spec.type
                                                                                                    === "slider"
                                                                                                    || spec.type
                                                                                                    === "text"))
        if (previousMode === "value-editing" && result.mode !== "value-editing")
            finishEdit()
        navigationMode = result.mode
        switch (result.effect) {
        case "toggle-sync":
            toggleSync(spec)
            return true
        case "begin-edit":
            beginEdit(spec, false)
            return true
        case "end-edit":
            return true
        case "activate":
            activateRow(list.currentIndex)
            return true
        case "value":
            return adjustRow(list.currentIndex, action === "right" ? 1 : -1)
        case "move-up":
        case "move-down":
            if (action === "up" && list.currentIndex <= list.firstEnabled(0, 1)) {
                if (!overVideo && shell)
                    shell.focusNavBar()
            } else {
                list.moveSelection(action === "up" ? -1 : 1)
            }
            return true
        case "back":
            return false
        default:
            return true
        }
    }

    function rowDescription(spec, selected) {
        const description = spec ? String(spec.description || "") : ""
        if (!selected || !hasSync(spec))
            return description
        return description + (description ? " " : "") + (spec.type === "slider" || spec.type === "text"
                                                         ? "OK to edit; Right for sync." : "Right for sync.")
    }

    function focusRow(index) {
        if (index !== list.currentIndex) {
            finishEdit()
            navigationMode = "row"
        }
        list.currentIndex = SettingsNavigation.clampIndex(index, list.count)
        list.clampEnabled()
        InputKeys.focus(list)
    }

    function beginReset() {
        const snapshot = {}
        for (let index = 0; index < appearanceKeys.length; ++index)
            snapshot[appearanceKeys[index]] = settingsController.values[appearanceKeys[index]]
        appearanceSnapshot = snapshot
        resetVisible = true
    }

    function confirmReset() {
        const snapshot = appearanceSnapshot
        settingsController.resetSubtitleAppearance()
        resetVisible = false
        InputKeys.focus(list)
        if (shell && shell.showToastAction) {
            shell.showToastAction("Subtitle appearance reset", "Undo", function () {
                for (let index = 0; index < root.appearanceKeys.length; ++index) {
                    const key = root.appearanceKeys[index]
                    root.settingsController.setValue(key, snapshot[key])
                }
            })
        }
    }

    function activateRow(index) {
        const entry = list.entryAt(index)
        if (!entry || entry.section)
            return
        const spec = entry.spec
        if (spec.type === "submenu") {
            advancedExpanded = !advancedExpanded
            rebuildRows(true)
        } else if (spec.type === "action") {
            beginReset()
        } else if (spec.type === "toggle") {
            setValue(spec, !Boolean(specValue(spec)), -1)
        } else if (spec.type === "select") {
            beginEdit(spec, false)
            list.positionViewAtIndex(index, ListView.Contain)
            Qt.callLater(function () {
                const anchor = root.rowControlAt(index)
                if (!anchor) {
                    root.finishEdit()
                    return
                }
                root.choiceRow = spec
                root.choiceAnchor = anchor
                root.choiceVisible = true
            })
        }
    }

    function adjustRow(index, direction) {
        const entry = list.entryAt(index)
        if (!entry || entry.section)
            return false
        const spec = entry.spec
        if (spec.type === "select") {
            activateRow(index)
            return true
        }
        if (spec.type === "slider") {
            const from = Number(spec.from || 0)
            const to = Number(spec.to || 100)
            const next = Math.max(from, Math.min(to, Number(specValue(spec)) + Number(spec.step || 1) * direction))
            setValue(spec, next, -1)
            return true
        }
        return false
    }

    function closeChoice() {
        finishEdit()
        choiceVisible = false
        choiceAnchor = null
        choiceRow = null
        Qt.callLater(function () {
            InputKeys.focus(list)
        })
    }

    // Pointer close. Over playback the panel owns its own visibility; as a
    // route it is the shell that has to pop back to Settings.
    function requestClose() {
        if (resetVisible || choiceVisible) {
            back()
            return
        }
        finishEdit()
        if (overVideo)
            dismissed()
        else if (shell && shell.back)
            shell.back()
    }

    function back() {
        if (resetVisible) {
            resetVisible = false
            InputKeys.focus(list)
            return true
        }
        if (choiceVisible) {
            closeChoice()
            return true
        }
        if (navigationMode !== "row")
            return routeAction("back")
        if (advancedExpanded) {
            advancedExpanded = false
            rebuildRows(true)
            return true
        }
        if (overVideo) {
            dismissed()
            return true
        }
        return false
    }

    function routeKey(key, phase, repeat) {
        if (resetVisible)
            return resetLoader.item.routeKey(key, phase, repeat)
        if (choiceVisible)
            return choiceLoader.item.routeKey(key, phase, repeat)
        const action = key === Qt.Key_Right ? "right" : key === Qt.Key_Left ? "left" : key === Qt.Key_Up ? "up" : key
                                                                                                           === Qt.Key_Down
                                                                                                           ? "down" :
                                                                                                             InputKeys.isAccept(
                                                                                                                 key) ? "activate" :
                                                                                                                        InputKeys.isBack(
                                                                                                                            key) ? "back" :
                                                                                                                                   ""
        if (!action)
            return false
        if (phase === "release")
            return true
        return action === "back" ? back() : routeAction(action)
    }

    function activate() {
        if (resetVisible)
            resetLoader.item.activate()
        else if (choiceVisible)
            choiceLoader.item.activate()
        else
            routeAction("activate")
    }

    focus: true
    onActiveFocusChanged: if (activeFocus)
                              focusRow(list.currentIndex)
    Component.onCompleted: {
        rebuildRows()
    }
    Component.onDestruction: finishEdit()
    onVisibleChanged: if (!visible)
                          finishEdit()

    Connections {
        target: root.settingsController
        ignoreUnknownSignals: true
        function onSettingsValuesChanged() {
            root.rebuildRows()
        }
        function onValuesChanged() {
            root.rebuildRows()
        }
        function onSettingsSchemaChanged() {
            root.rebuildRows()
        }
    }

    onHdrPlaybackChanged: Qt.callLater(root.rebuildRows)

    Connections {
        target: root.syncController
        function onStatesChanged() {
            const entry = list.entryAt(list.currentIndex)
            if (root.navigationMode === "sync-action" && (!entry || !root.hasSync(entry.spec)))
                root.navigationMode = "row"
        }
    }

    Surface {
        anchors.left: list.left
        anchors.right: list.right
        anchors.top: heading.top
        anchors.bottom: list.bottom
        anchors.margins: -Metrics.scaled(14)
        visible: root.overVideo
        baseColor: Theme.floatingPanel
        elevated: true
    }

    Item {
        id: heading

        anchors.top: parent.top
        anchors.left: list.left
        anchors.right: list.right
        anchors.topMargin: list.inset
        height: Math.max(closeButton.height, headingText.implicitHeight)

        AppText {
            id: headingText
            anchors.left: parent.left
            anchors.right: closeButton.visible ? closeButton.left : parent.right
            anchors.rightMargin: Metrics.scaled(12)
            anchors.verticalCenter: parent.verticalCenter
            text: "Subtitle appearance"
            font.pixelSize: Metrics.titleSizePx
            font.weight: Font.DemiBold
            maximumLineCount: 1
            elide: Text.ElideRight
        }

        // Pointer-only affordance: a remote closes with Back, and a focusable
        // button up here would just be one more stop on the way to the rows.
        IconButton {
            id: closeButton
            visible: !root.platformInfo.isTV
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            focusPolicy: Qt.NoFocus
            chromeless: true
            iconName: "close"
            accessibleName: "Close subtitle appearance"
            onClicked: root.requestClose()
        }
    }

    ListModel {
        id: rowsModel
        dynamicRoles: true
    }

    MenuListView {
        id: list

        readonly property real inset: Metrics.pageMarginPx

        // As a sheet the panel hugs the right edge and stays narrow, so the
        // subtitles it is changing remain on screen.
        width: root.overVideo ? Math.min(parent.width * 0.42, Metrics.scaled(620)) : Math.max(0, parent.width - inset
                                                                                              * 2)

        anchors.top: heading.bottom
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.topMargin: Metrics.scaled(12)
        anchors.bottomMargin: inset
        anchors.rightMargin: inset
        model: rowsModel
        entryProvider: function (index) {
            return index >= 0 && index < root.rows.length ? root.rows[index] : null
        }
        spacing: Metrics.scaled(10)
        dismissOnBack: false
        dismissOnHorizontal: false
        header: root.overVideo ? null : previewComponent
        headerPositioning: ListView.InlineHeader
        bottomMargin: root.choiceVisible && choiceLoader.item ? choiceLoader.item.panelHeight + Metrics.scaled(16) : 0
        onCountChanged: {
            if (root.pendingFocusIndex < 0 || count !== root.rows.length)
                return
            const targetIndex = root.pendingFocusIndex
            root.pendingFocusIndex = -1
            Qt.callLater(function () {
                root.focusRow(targetIndex)
            })
        }
        onAccepted: index => root.routeAction("activate")
        onEdgeUp: if (!root.overVideo && root.shell)
                      root.shell.focusNavBar()

        delegate: Item {
            id: delegateItem

            required property int index
            required property var spec
            required property bool section

            readonly property bool isSection: section === true
            readonly property Item control: rowLoader.item
            // The view guarantees a single current delegate; a per-row copy of
            // the index does not survive the model changing shape underneath
            // it, and two rows can then answer to the same currentIndex.
            readonly property bool rowCurrent: ListView.isCurrentItem && list.activeFocus

            width: list.width
            implicitHeight: isSection ? sectionHeader.implicitHeight + Metrics.scaled(18) : Math.max(rowLoader.implicitHeight,
                                                                                                     syncControl.implicitHeight)
            height: implicitHeight

            GroupHeader {
                id: sectionHeader
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                visible: delegateItem.isSection
                title: delegateItem.isSection ? delegateItem.spec.title : ""
            }

            Loader {
                id: rowLoader
                anchors.left: parent.left
                anchors.right: syncControl.visible ? syncControl.left : parent.right
                anchors.rightMargin: syncControl.visible ? Metrics.scaled(8) : 0
                anchors.verticalCenter: parent.verticalCenter
                active: !delegateItem.isSection
                // Bound rather than assigned once at load: the loaded row
                // reads these back through its parent, so it keeps up when
                // the model shifts around a delegate that outlives it.
                readonly property var spec: delegateItem.spec
                readonly property int rowIndex: delegateItem.index
                readonly property bool rowCurrent: delegateItem.rowCurrent
                sourceComponent: delegateItem.isSection ? null : delegateItem.spec.type === "toggle" ? toggleComponent :
                                                                                                       delegateItem.spec.type
                                                                                                       === "slider"
                                                                                                       ? sliderComponent :
                                                                                                         delegateItem.spec.type
                                                                                                         === "select"
                                                                                                         ? selectComponent :
                                                                                                           actionComponent
            }

            SettingSyncControl {
                id: syncControl
                objectName: "subtitleSync/" + settingKey
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: !delegateItem.isSection && root.hasSync(delegateItem.spec)
                settingKey: delegateItem.spec.key || ""
                settingTitle: delegateItem.spec.title || ""
                syncState: root.syncState(delegateItem.spec)
                sourceLabel: root.syncSourceLabel
                actionFocused: delegateItem.rowCurrent && root.navigationMode === "sync-action"
                helpFocused: delegateItem.rowCurrent
                onToggled: {
                    root.focusRow(delegateItem.index)
                    root.finishEdit()
                    root.toggleSync(delegateItem.spec)
                }
            }
        }
    }

    // Without video behind it there is nothing honest to preview against, so
    // show one plain band that is light on one side and dark on the other.
    Component {
        id: previewComponent

        Item {
            width: list.width
            height: band.height + Metrics.scaled(18)

            Rectangle {
                id: band
                width: parent.width
                height: Math.max(Metrics.scaled(96), sampleText.implicitHeight + Metrics.scaled(48))
                radius: Theme.radiusLarge
                color: "#c8c8c8"
                clip: true

                Rectangle {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: parent.width / 2
                    color: "#101010"
                }

                Rectangle {
                    anchors.centerIn: sampleText
                    width: sampleText.width + Metrics.scaled(24)
                    height: sampleText.height + Metrics.scaled(12)
                    radius: Theme.radiusSmall
                    color: {
                        const value = String(root.settingsController.values["subtitles/textBackground"]
                                             || "transparent")
                        return value === "opaque" ? "#ff000000" : value === "translucent" ? "#a0000000" : "transparent"
                    }
                }

                AppText {
                    id: sampleText
                    anchors.centerIn: parent
                    width: parent.width - Metrics.scaled(48)
                    text: "We can read this on either side."
                    horizontalAlignment: Text.AlignHCenter
                    font.family: {
                        const value = String(root.settingsController.values["subtitles/font"] || "")
                        if (value.indexOf("system:") === 0)
                            return value.slice(7)
                        return value === "interface" ? Typography.sans : Typography.subtitle
                    }
                    font.pixelSize: Metrics.scaled(26) * Number(
                                        root.settingsController.values["subtitles/scalePercent"] || 100) / 100
                    font.weight: root.settingsController.values["subtitles/textWeight"] === "bold" ? Font.Bold :
                                                                                                     Font.Normal

                    style: root.settingsController.values["subtitles/dropShadow"] === "none" ? Text.Normal :
                                                                                               Text.Outline

                    styleColor: "#e6000000"
                    color: root.settingsController.values["subtitles/textColor"] || "white"
                }
            }
        }
    }

    Component {
        id: actionComponent
        SettingRow {
            id: actionRow
            readonly property var spec: parent ? parent.spec : null
            readonly property int rowIndex: parent ? parent.rowIndex : -1
            // The chevron says "this row unfolds in place" the way the select
            // rows do, so the submenu needs no word for it.
            readonly property bool isSubmenu: spec !== undefined && spec !== null && spec.type === "submenu"
            width: parent ? parent.width : 0
            focus: false
            focusPolicy: Qt.NoFocus
            rowFocus: parent ? parent.rowCurrent : false
            title: spec ? spec.title : ""
            description: spec ? spec.description : ""
            valueText: isSubmenu ? "" : "Reset"
            valueTextVisible: !isSubmenu
            trailing: [
                MaterialIcon {
                    visible: actionRow.isSubmenu
                    name: root.advancedExpanded ? "expand_less" : "expand_more"
                    iconSize: Math.max(20, Metrics.iconSizePx)
                    iconColor: Theme.textSecondary
                }
            ]
            onClicked: {
                root.focusRow(rowIndex)
                root.activateRow(rowIndex)
            }
        }
    }

    Component {
        id: toggleComponent
        ToggleRow {
            readonly property var spec: parent ? parent.spec : null
            readonly property int rowIndex: parent ? parent.rowIndex : -1
            width: parent ? parent.width : 0
            focus: false
            focusPolicy: Qt.NoFocus
            rowFocus: parent ? parent.rowCurrent : false
            title: spec ? spec.title : ""
            description: root.rowDescription(spec, rowFocus)
            checked: spec ? Boolean(root.specValue(spec)) : false
            onToggled: checked => {
                root.focusRow(rowIndex)
                root.setValue(spec, checked, -1)
            }
        }
    }

    Component {
        id: selectComponent
        SelectRow {
            readonly property var spec: parent ? parent.spec : null
            readonly property int rowIndex: parent ? parent.rowIndex : -1
            width: parent ? parent.width : 0
            focus: false
            focusPolicy: Qt.NoFocus
            rowFocus: parent ? parent.rowCurrent : false
            title: spec ? spec.title : ""
            description: root.rowDescription(spec, rowFocus)
            options: spec ? root.choiceLabels(spec) : []
            currentIndex: spec ? root.currentChoice(spec) : 0
            onOpened: {
                root.focusRow(rowIndex)
                root.activateRow(rowIndex)
            }
            onSelected: index => root.setChoice(spec, index)
        }
    }

    Component {
        id: sliderComponent
        SliderRow {
            id: sliderRow
            readonly property var spec: parent ? parent.spec : null
            readonly property int rowIndex: parent ? parent.rowIndex : -1
            width: parent ? parent.width : 0
            selected: parent ? parent.rowCurrent : false
            title: spec ? spec.title : ""
            description: root.rowDescription(spec, selected)
            from: spec ? Number(spec.from) : 0
            to: spec ? Number(spec.to) : 100
            step: spec ? Number(spec.step || 1) : 1
            unitText: spec ? String(spec.unitText || "") : ""
            value: spec ? Number(root.specValue(spec)) : 0
            onValuePreviewed: value => root.settingsController.previewValue(spec.key, value)
            onValueEdited: value => {
                if (root.editingKey !== spec.key)
                    root.beginEdit(spec, true)
                root.setValue(spec, value, -1)
                if (root.pointerEditing)
                    root.finishEdit()
            }
            onInteractionStarted: {
                list.currentIndex = rowIndex
                root.beginEdit(spec, true)
            }

            Connections {
                target: sliderRow.trailing[0]
                function onDraggingChanged() {
                    if (target.dragging)
                        return
                    // Release commits synchronously; a cancelled grab has no
                    // commit and must release its sync edit lock as well.
                    Qt.callLater(function () {
                        if (root.pointerEditing && root.editingKey === sliderRow.spec.key)
                            root.finishEdit()
                    })
                }
            }
        }
    }

    Loader {
        id: resetLoader
        anchors.fill: parent
        active: root.resetVisible
        z: 200
        sourceComponent: ConfirmationDialog {
            title: "Reset subtitle appearance?"
            message: "Font, colour, size, position, and HDR settings all go back to their defaults."
            confirmText: "Reset"
            destructive: true
            onAccepted: root.confirmReset()
            onDismissed: {
                root.resetVisible = false
                InputKeys.focus(list)
            }
        }
    }

    Loader {
        id: choiceLoader
        anchors.fill: parent
        active: root.choiceVisible
        sourceComponent: OptionPickerDialog {
            visible: true
            anchorItem: root.choiceAnchor
            title: root.choiceRow ? root.choiceRow.title : "Choose an option"
            options: root.choiceRow ? root.choiceLabels(root.choiceRow) : []
            currentIndex: root.choiceRow ? root.currentChoice(root.choiceRow) : 0
            onSelected: index => {
                if (root.choiceRow)
                    root.setChoice(root.choiceRow, index)
                root.closeChoice()
            }
            onDismissed: root.closeChoice()
            onSpaceBelowRequired: pixels => {
                const maximum = Math.max(0, list.contentHeight + list.bottomMargin - list.height)
                list.contentY = Math.min(maximum, Math.max(0, list.contentY + pixels))
                Qt.callLater(choiceLoader.item.completePresentation)
            }
        }
    }
}
