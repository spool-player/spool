pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

// Choosing where media comes from: official providers first, then the
// community's, then any provider by link. Installing and signing in are one
// step from here, and the same page updates or removes what is installed.
// A curated build has no link row, and a bundled one only its own providers.
FocusScope {
    id: root

    property var shell
    property string pendingSetup: ""
    property bool linkEditing: false
    property var menuEntry: null
    property var removingEntry: null

    // Section headers only once there is more than one section.
    readonly property var rows: {
        const community = Store.storeAvailable && (Store.community.length > 0 || Store.loading)
        const out = community ? [
                                    {
                                        "kind": "header",
                                        "title": "By Spool"
                                    }
                                ] : []
        out.push(...Store.official)
        if (community) {
            out.push({
                         "kind": "header",
                         "title": "Community",
                         "loading": Store.loading
                     })
            out.push(...Store.community)
        }
        if (Store.storeAvailable && Store.linksAllowed)
            out.push({
                         "kind": "link",
                         "id": "link"
                     })
        return out
    }

    // Headers come and go as the store loads; stay on the same entry.
    property string currentId: ""
    onRowsChanged: {
        const index = rows.findIndex(row => row.kind !== "header" && row.id === currentId)
        list.currentIndex = index >= 0 ? index : step(-1, 1)
    }

    function step(from, delta) {
        let index = from + delta
        while (index >= 0 && index < rows.length && rows[index].kind === "header")
            index += delta
        return index >= 0 && index < rows.length ? index : from
    }

    focus: true
    Component.onCompleted: {
        Store.refresh(true)
        list.currentIndex = step(-1, 1)
        InputKeys.focus(list)
    }

    function stateOf(entry) {
        if (entry.kind === "link")
            return Store.busy.url ? "busy" : "link"
        if (entry.busy)
            return "busy"
        if (!entry.compatible)
            return "incompatible"
        if (entry.updateAvailable)
            return "update"
        return entry.installed ? "installed" : "get"
    }

    function beginSetup(id) {
        pendingSetup = ""
        const context = Providers.beginSetup(id)
        if (context)
            shell.openProviderScreen(context)
    }

    function activateRow(index) {
        const entry = rows[index]
        if (!entry)
            return
        switch (stateOf(entry)) {
        case "link":
            linkEditing = true
            Qt.callLater(() => {
                const item = list.itemAtIndex(index)
                if (item)
                    item.focusField()
            })
            break
        case "update":
            pendingSetup = entry.id
            Store.update(entry.id)
            break
        case "installed":
            beginSetup(entry.id)
            break
        case "get":
            pendingSetup = entry.id
            Store.install(entry.id)
            break
        }
    }

    function submitLink(text) {
        const url = String(text || "").trim()
        if (url.length === 0)
            return
        pendingSetup = "*"
        Store.addFromUrl(url)
        linkEditing = false
        InputKeys.focus(list)
    }

    function openMenu(index) {
        const entry = rows[index]
        if (!entry || !entry.installed)
            return false
        menuEntry = entry
        menuLoader.active = true
        return true
    }

    readonly property var menuActions: {
        const entry = menuEntry
        if (!entry)
            return []
        const out = [
                  {
                      "label": "Add an account",
                      "value": "setup"
                  }
              ]
        if (entry.updateAvailable)
            out.push({
                         "label": "Update to " + entry.version,
                         "value": "update"
                     })
        if (entry.removable !== false && !entry.bundled)
            out.push({
                         "label": "Remove",
                         "value": "remove"
                     })
        return out
    }

    function choose(index) {
        const entry = menuEntry
        const action = menuActions[index] ? menuActions[index].value : ""
        closeMenu()
        if (action === "setup")
            beginSetup(entry.id)
        else if (action === "update")
            Store.update(entry.id)
        else if (action === "remove")
            removingEntry = entry
    }

    function closeMenu() {
        menuLoader.active = false
        InputKeys.focus(list)
    }

    function activate() {
        if (removeConfirmation.item)
            return removeConfirmation.item.activate()
        if (menuLoader.item)
            return menuLoader.item.activate()
        activateRow(list.currentIndex)
    }

    function longPress() {
        return openMenu(list.currentIndex)
    }

    function back() {
        if (removingEntry) {
            removingEntry = null
            InputKeys.focus(list)
            return true
        }
        if (menuLoader.item) {
            closeMenu()
            return true
        }
        if (linkEditing) {
            linkEditing = false
            InputKeys.focus(list)
            return true
        }
        return false
    }

    function routeKey(key, phase, repeat) {
        if (removeConfirmation.item)
            return removeConfirmation.item.routeKey(key, phase, repeat)
        if (menuLoader.item)
            return menuLoader.item.routeKey(key, phase, repeat)
        if (linkEditing)
            return false
        if (phase !== "press")
            return InputKeys.isDirection(key)
        if (key === Qt.Key_Down)
            list.currentIndex = step(list.currentIndex, 1)
        else if (key === Qt.Key_Up)
            list.currentIndex = step(list.currentIndex, -1)
        else if (key === Qt.Key_Menu || key === Qt.Key_M)
            return openMenu(list.currentIndex)
        else
            return InputKeys.isDirection(key)
        return true
    }

    Connections {
        target: Store
        function onInstalled(id) {
            if (root.pendingSetup === id || root.pendingSetup === "*")
                root.beginSetup(id)
        }
        function onProblem() {
            root.pendingSetup = ""
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    Item {
        id: column
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width - Metrics.pageMarginPx * 2, Metrics.scaled(880))

        ColumnLayout {
            id: header
            y: Math.max(Metrics.pageMarginPx, Metrics.scaled(56))
            width: parent.width
            spacing: Metrics.scaled(10)

            RowLayout {
                Layout.fillWidth: true
                spacing: Metrics.scaled(14)

                IconButton {
                    visible: Providers.hasAccounts
                    iconName: "arrow_back"
                    accessibleName: "Back"
                    onClicked: Router.pop("accounts")
                }

                AppText {
                    Layout.fillWidth: true
                    text: "Add a provider"
                    font.pixelSize: Metrics.scaled(44)
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
            }

            AppText {
                Layout.fillWidth: true
                text: "Choose where your media comes from. You can add as many as you like."
                color: Theme.textSecondary
                font.pixelSize: Metrics.scaled(20)
                wrapMode: Text.WordWrap
            }
            AppText {
                Layout.fillWidth: true
                visible: Store.error.length > 0
                text: Store.error
                color: Theme.errorText
                font.pixelSize: Metrics.bodySizePx
                wrapMode: Text.Wrap
            }
        }

        ListView {
            id: list
            anchors.top: header.bottom
            anchors.topMargin: Metrics.scaled(32)
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            clip: true
            model: root.rows
            currentIndex: 0
            onCurrentIndexChanged: root.currentId = root.rows[currentIndex] ? String(root.rows[currentIndex].id || "") :
                                                                              ""
            focus: true
            keyNavigationEnabled: false
            boundsBehavior: Flickable.StopAtBounds
            highlightMoveDuration: 0
            spacing: Metrics.scaled(14)
            footer: Item {
                width: list.width
                height: Metrics.pageMarginPx
            }

            delegate: Item {
                id: slot
                required property int index
                required property var modelData
                readonly property bool isHeader: modelData.kind === "header"

                function focusField() {
                    linkField.focusRow()
                }

                width: list.width
                height: isHeader ? Metrics.scaled(index === 0 ? 32 : 56) : row.height

                AppText {
                    visible: slot.isHeader
                    anchors.left: parent.left
                    anchors.leftMargin: Metrics.scaled(4)
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: Metrics.scaled(4)
                    text: String(slot.modelData.title || "").toUpperCase()
                    color: Theme.textMuted
                    font.pixelSize: Metrics.scaled(15)
                    font.weight: Font.DemiBold
                    font.letterSpacing: Metrics.scaled(1)
                }

                BusySpinner {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    width: Metrics.scaled(18)
                    height: width
                    running: slot.isHeader && Boolean(slot.modelData.loading)
                    visible: running
                }

                Rectangle {
                    id: row
                    visible: !slot.isHeader
                    readonly property int index: slot.index
                    readonly property var modelData: slot.modelData
                    readonly property string state: root.stateOf(modelData)
                    readonly property bool current: slot.ListView.isCurrentItem && Metrics.keyboardFocusActive
                                                    && list.activeFocus
                    readonly property bool isLink: modelData.kind === "link"

                    width: list.width
                    height: isLink && root.linkEditing ? linkForm.implicitHeight + Metrics.scaled(40) : Math.max(
                                                             Metrics.touchTargetPx, Metrics.scaled(104))
                    radius: Theme.radiusPanel
                    color: current ? Theme.focusedFill : hover.hovered ? Theme.bgHover : Theme.bgRaised
                    border.width: current ? Theme.focusBorderWidth : Theme.hoverBorderWidth
                    border.color: current ? Theme.accent : Theme.border

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Metrics.scaled(24)
                        anchors.rightMargin: Metrics.scaled(24)
                        spacing: Metrics.scaled(22)
                        visible: !(row.isLink && root.linkEditing)

                        Loader {
                            Layout.preferredWidth: Metrics.scaled(64)
                            Layout.preferredHeight: Metrics.scaled(64)
                            sourceComponent: row.isLink ? linkIcon : providerIcon
                            Component {
                                id: providerIcon
                                ProviderIcon {
                                    source: row.modelData.iconUrl || ""
                                    name: row.modelData.name || ""
                                    seed: row.modelData.id || ""
                                    // The folders provider is built in and has no package icon.
                                    symbol: row.modelData.id === "spool.local" ? "computer" : ""
                                }
                            }
                            Component {
                                id: linkIcon
                                Rectangle {
                                    radius: Math.round(width * 0.22)
                                    color: Theme.bgPanel
                                    MaterialIcon {
                                        anchors.centerIn: parent
                                        name: "link"
                                        iconSize: Metrics.scaled(32)
                                        iconColor: Theme.textSecondary
                                    }
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Metrics.scaled(4)

                            AppText {
                                Layout.fillWidth: true
                                text: row.isLink ? "Add from a link" : row.modelData.name || ""
                                font.pixelSize: Metrics.scaled(24)
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                                maximumLineCount: 1
                            }

                            SecondaryText {
                                Layout.fillWidth: true
                                text: row.state === "incompatible" ? "Needs a newer Spool" : row.isLink
                                                                     ? "Install a provider someone has shared with you" :
                                                                       row.modelData.summary || ""
                                visible: text.length > 0
                                color: Theme.textSecondary
                                font.pixelSize: Metrics.scaled(17)
                                elide: Text.ElideRight
                                maximumLineCount: 1
                            }
                        }

                        // One action per row, named for what pressing the row does.
                        Rectangle {
                            Layout.preferredHeight: Metrics.scaled(44)
                            Layout.preferredWidth: Math.max(Metrics.scaled(120), chipText.implicitWidth + Metrics.scaled(
                                                                40))
                            visible: row.state !== "incompatible" && row.state !== "busy"
                            radius: height / 2
                            color: row.state === "get" || row.state === "update" ? Theme.accentDim : Theme.bgPanel
                            border.width: Theme.hoverBorderWidth
                            border.color: row.state === "get" || row.state === "update" ? Theme.accentDim : Theme.border

                            AppText {
                                id: chipText
                                anchors.centerIn: parent
                                text: ({
                                           "get": "Install",
                                           "update": "Update",
                                           "installed": "Add account",
                                           "link": "Paste link"
                                       })[row.state] || ""
                                font.pixelSize: Metrics.scaled(17)
                                font.weight: Font.DemiBold
                            }
                        }

                        BusySpinner {
                            Layout.preferredWidth: Metrics.scaled(24)
                            Layout.preferredHeight: Metrics.scaled(24)
                            running: row.state === "busy"
                            visible: running
                        }
                    }

                    ColumnLayout {
                        id: linkForm
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: Metrics.scaled(20)
                        visible: row.isLink && root.linkEditing
                        spacing: Metrics.scaled(10)

                        TextFieldRow {
                            id: linkField
                            Layout.fillWidth: true
                            label: "Link"
                            placeholderText: "https://github.com/you/spool-provider"
                            inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                            onAccepted: root.submitLink(text)
                        }

                        RowLayout {
                            Layout.alignment: Qt.AlignRight
                            spacing: Metrics.scaled(10)
                            ActionButton {
                                text: "Cancel"
                                kind: "flat"
                                onClicked: root.back()
                            }
                            ActionButton {
                                text: "Add"
                                kind: "primary"
                                enabled: linkField.text.trim().length > 0
                                onClicked: root.submitLink(linkField.text)
                            }
                        }
                    }

                    HoverHandler {
                        id: hover
                        enabled: !(row.isLink && root.linkEditing)
                    }

                    TapHandler {
                        enabled: !(row.isLink && root.linkEditing)
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onTapped: (point, button) => {
                            list.currentIndex = row.index
                            if (button === Qt.RightButton)
                                root.openMenu(row.index)
                            else
                                root.activateRow(row.index)
                        }
                        onLongPressed: root.openMenu(row.index)
                    }
                }
            }
        }
    }

    Loader {
        id: menuLoader
        anchors.fill: parent
        active: false
        sourceComponent: OptionPickerDialog {
            visible: true
            anchorItem: list.currentItem
            title: root.menuEntry ? root.menuEntry.name : ""
            options: root.menuActions.map(action => action.label)
            onSelected: index => root.choose(index)
            onDismissed: root.closeMenu()
        }
    }
    Loader {
        id: removeConfirmation
        anchors.fill: parent
        active: Boolean(root.removingEntry)
        z: 210
        sourceComponent: ConfirmationDialog {
            title: "Remove provider?"
            message: "Remove " + root.removingEntry.name
                     + " and its saved accounts from this device? Your server accounts and media will not be deleted."
            confirmText: "Remove provider"
            destructive: true
            onAccepted: {
                Store.uninstall(root.removingEntry.id)
                root.removingEntry = null
                InputKeys.focus(list)
            }
            onDismissed: {
                root.removingEntry = null
                InputKeys.focus(list)
            }
        }
    }
}
