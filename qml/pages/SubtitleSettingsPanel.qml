pragma ComponentBehavior: Bound

import QtQuick
import "../theme"
import "../primitives"
import "SettingsNavigation.js" as SettingsNavigation

FocusScope {
    id: root

    property var shell
    property var uiTransitionToken: 0
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

    readonly property var sections: [
        {
            "title": "Size and position",
            "keys": ["subtitles/scalePercent", "subtitles/verticalPositionPercent",
                "subtitles/alwaysOverridePositionAndSize", "subtitles/allowInBlackBars"]
        },
        {
            "title": "Colour",
            "keys": ["subtitles/overrideTextColor", "subtitles/textColor"]
        },
        {
            "title": "Which subtitles",
            "keys": ["subtitles/language", "subtitles/mode"]
        }
    ]
    readonly property var advancedSections: [
        {
            "title": "Text style",
            "keys": ["subtitles/styling", "subtitles/textWeight", "subtitles/font", "subtitles/dropShadow",
                "subtitles/textBackground"]
        },
        {
            "title": "Image subtitles",
            "keys": ["subtitles/recolorImageSubtitles", "subtitles/bitmapSharpnessPercent",
                "subtitles/bitmapShadowEnabled"]
        },
        {
            "title": "Image subtitle shadow",
            "keys": ["subtitles/bitmapShadowCoreSize", "subtitles/bitmapShadowCoreGrow",
                "subtitles/bitmapShadowCoreOpacityPercent", "subtitles/bitmapShadowSpreadEnabled",
                "subtitles/bitmapShadowSpreadSize", "subtitles/bitmapShadowSpreadGrow", "subtitles/bitmapShadowSpreadX",
                "subtitles/bitmapShadowSpreadY", "subtitles/bitmapShadowSpreadOpacityPercent",
                "subtitles/bitmapShadowDither"]
        },
        {
            "title": "HDR",
            "keys": ["subtitles/hdrBrightnessPercent"]
        },
        {
            "title": "Start over",
            "keys": ["action/resetSubtitleAppearance"]
        }
    ]

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
            return Settings.subtitleLanguageIndex
        const value = Settings.values[spec.key]
        return value === undefined || value === null ? spec.defaultValue : value
    }

    // Desktop can offer whatever fonts are installed on top of the bundled ones.
    function expandedSpec(spec) {
        if (!spec || spec.key !== "subtitles/font" || !Platform.hasSystemFonts)
            return spec
        const expanded = Object.assign({}, spec)
        expanded.choiceLabels = spec.choiceLabels.slice()
        expanded.choiceValues = spec.choiceValues.slice()
        const families = Settings.systemSubtitleFonts
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
        const schema = Settings.settingsSchema
        const byKey = {}
        for (let index = 0; index < schema.length; ++index)
            byKey[schema[index].key] = schema[index]

        const resolve = function (key) {
            const spec = byKey[key]
            const available = SettingsNavigation.rowAvailable(spec, Platform, Player.hdrPlayback, function (name) {
                const value = Settings.values[name]
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
        rows = visibleRows
    }

    function choiceLabels(spec) {
        if (spec.key === "subtitles/language")
            return Settings.subtitleLanguageOptions
        // Some labels carry a "%1" placeholder for the preferred language name.
        if (spec.key === "subtitles/mode") {
            const options = Settings.subtitleLanguageOptions
            const index = Settings.subtitleLanguageIndex
            const word = index > 0 && index < options.length ? String(options[index]).split(" ")[0] : "your language"
            const result = []
            for (let i = 0; i < spec.choiceLabels.length; ++i)
                result.push(String(spec.choiceLabels[i]).replace("%1", word))
            return result
        }
        return spec.choiceLabels || []
    }

    function choiceValues(spec) {
        return spec.key === "subtitles/language" ? Settings.subtitleLanguageOptions : (spec.choiceValues || [])
    }

    function currentChoice(spec) {
        if (spec.key === "subtitles/language")
            return Settings.subtitleLanguageIndex
        const values = choiceValues(spec)
        const value = String(specValue(spec))
        for (let index = 0; index < values.length; ++index)
            if (String(values[index]) === value)
                return index
        return 0
    }

    function setValue(spec, value, index) {
        if (spec.key === "subtitles/language")
            Settings.setSubtitleLanguageIndex(index)
        else
            Settings.setValue(spec.key, value)
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

    function focusRow(index) {
        list.currentIndex = SettingsNavigation.clampIndex(index, list.count)
        list.clampEnabled()
        InputKeys.focus(list)
    }

    function beginReset() {
        const snapshot = {}
        for (let index = 0; index < appearanceKeys.length; ++index)
            snapshot[appearanceKeys[index]] = Settings.values[appearanceKeys[index]]
        appearanceSnapshot = snapshot
        resetVisible = true
    }

    function confirmReset() {
        const snapshot = appearanceSnapshot
        Settings.resetSubtitleAppearance()
        resetVisible = false
        InputKeys.focus(list)
        if (shell && shell.showToastAction) {
            shell.showToastAction("Subtitle appearance reset", "Undo", function () {
                for (let index = 0; index < root.appearanceKeys.length; ++index) {
                    const key = root.appearanceKeys[index]
                    Settings.setValue(key, snapshot[key])
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
            list.positionViewAtIndex(index, ListView.Contain)
            Qt.callLater(function () {
                const anchor = root.rowControlAt(index)
                if (!anchor)
                    return
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
        if (phase === "release" && InputKeys.isDirection(key))
            return true
        if (InputKeys.isHorizontal(key) && adjustRow(list.currentIndex, key === Qt.Key_Right ? 1 : -1))
            return true
        if (key === Qt.Key_Up && list.currentIndex <= list.firstEnabled(0, 1)) {
            if (!overVideo && shell)
                shell.focusNavBar()
            return true
        }
        return list.routeKey(key, phase, repeat)
    }

    function activate() {
        if (resetVisible)
            resetLoader.item.activate()
        else if (choiceVisible)
            choiceLoader.item.activate()
        else
            activateRow(list.currentIndex)
    }

    focus: true
    onActiveFocusChanged: if (activeFocus)
                              focusRow(list.currentIndex)
    Component.onCompleted: {
        rebuildRows()
    }

    Connections {
        target: Player
        function onHdrPlaybackChanged() {
            root.rebuildRows()
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
            visible: !Platform.isTV
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            focusPolicy: Qt.NoFocus
            chromeless: true
            iconName: "close"
            accessibleName: "Close subtitle appearance"
            onClicked: root.requestClose()
        }
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
        model: root.rows
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
        onAccepted: index => root.activateRow(index)
        onEdgeUp: if (!root.overVideo && root.shell)
                      root.shell.focusNavBar()

        delegate: Item {
            id: delegateItem

            required property int index
            required property var modelData

            readonly property var spec: modelData.spec
            readonly property bool isSection: modelData.section === true
            readonly property Item control: rowLoader.item
            // The view guarantees a single current delegate; a per-row copy of
            // the index does not survive the model changing shape underneath
            // it, and two rows can then answer to the same currentIndex.
            readonly property bool rowCurrent: ListView.isCurrentItem && list.activeFocus

            width: list.width
            implicitHeight: isSection ? sectionHeader.implicitHeight + Metrics.scaled(18) : rowLoader.implicitHeight
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
                anchors.fill: delegateItem.isSection ? undefined : parent
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
                        const value = String(Settings.values["subtitles/textBackground"] || "transparent")
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
                        const value = String(Settings.values["subtitles/font"] || "")
                        if (value.indexOf("system:") === 0)
                            return value.slice(7)
                        return value === "interface" ? Typography.sans : Typography.subtitle
                    }
                    font.pixelSize: Metrics.scaled(26) * Number(Settings.values["subtitles/scalePercent"] || 100) / 100
                    font.weight: Settings.values["subtitles/textWeight"] === "bold" ? Font.Bold : Font.Normal
                    style: Settings.values["subtitles/dropShadow"] === "none" ? Text.Normal : Text.Outline
                    styleColor: "#e6000000"
                    color: Settings.values["subtitles/textColor"] || "white"
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
            width: list.width
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
            width: list.width
            focus: false
            focusPolicy: Qt.NoFocus
            rowFocus: parent ? parent.rowCurrent : false
            title: spec ? spec.title : ""
            description: spec ? spec.description : ""
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
            width: list.width
            focus: false
            focusPolicy: Qt.NoFocus
            rowFocus: parent ? parent.rowCurrent : false
            title: spec ? spec.title : ""
            description: spec ? spec.description : ""
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
            readonly property var spec: parent ? parent.spec : null
            readonly property int rowIndex: parent ? parent.rowIndex : -1
            width: list.width
            selected: parent ? parent.rowCurrent : false
            title: spec ? spec.title : ""
            description: spec ? spec.description : ""
            from: spec ? Number(spec.from) : 0
            to: spec ? Number(spec.to) : 100
            step: spec ? Number(spec.step || 1) : 1
            unitText: spec ? String(spec.unitText || "") : ""
            value: spec ? Number(root.specValue(spec)) : 0
            onValuePreviewed: value => Settings.previewValue(spec.key, value)
            onValueEdited: value => root.setValue(spec, value, -1)
            onInteractionStarted: root.focusRow(rowIndex)
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
