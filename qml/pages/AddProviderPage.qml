pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

// Adding an account: pick the service your media is on and sign in. Whether
// that service's provider still has to be downloaded, or updated first, is
// Spool's business, not the viewer's -- choosing a tile does whatever it
// takes and then opens its sign-in. Spool's own services come first, then
// the community's; a provider shared as a link is one quiet button below.
FocusScope {
    id: root

    property var shell
    property string pendingSetup: ""
    property bool linkEditing: false
    property var menuEntry: null
    property var removingEntry: null
    // "grid" or "link": where remote focus is.
    property string focusArea: "grid"
    property string problemText: ""

    readonly property bool firstAccount: !Providers.hasAccounts
    readonly property var entries: {
        const out = Store.official.map(entry => Object.assign({}, entry, {
                                                                  "community": false
                                                              }))
        if (Store.storeAvailable)
            out.push(...Store.community.map(entry => Object.assign({}, entry, {
                                                                       "community": true
                                                                   })))
        return out
    }
    readonly property bool linkAvailable: Store.storeAvailable && Store.linksAllowed

    // The store loads in pieces; stay on the same service as it does.
    property string currentId: ""
    onEntriesChanged: {
        const index = entries.findIndex(entry => entry.id === currentId)
        tiles.currentIndex = index >= 0 ? index : Math.min(Math.max(0, tiles.currentIndex), entries.length - 1)
    }

    focus: true
    Component.onCompleted: {
        Store.refresh(true)
        tiles.currentIndex = 0
        InputKeys.focus(tiles)
    }

    function stateOf(entry) {
        if (!entry)
            return ""
        if (entry.busy || pendingSetup === entry.id)
            return "busy"
        if (!entry.compatible)
            return entry.installed ? "ready" : "unavailable"
        return "ready"
    }

    function accountCount(entry) {
        return Providers.accounts.filter(account => account.moduleId === entry.id).length
    }

    function detailText(entry) {
        switch (stateOf(entry)) {
        case "busy":
            return "Getting ready…"
        case "unavailable":
            return "Needs a newer version of Spool"
        }
        const accounts = accountCount(entry)
        if (accounts > 0)
            return accounts === 1 ? "1 account added · Add another" : accounts + " accounts added · Add another"
        return entry.summary || ""
    }

    function beginSetup(id) {
        pendingSetup = ""
        const context = Providers.beginSetup(id)
        if (context)
            shell.openProviderScreen(context)
    }

    // One action whatever the provider's state: get it current, then sign in.
    function activateEntry(index) {
        const entry = entries[index]
        if (!entry || stateOf(entry) !== "ready")
            return
        problemText = ""
        if (!entry.installed) {
            pendingSetup = entry.id
            Store.install(entry.id)
        } else if (entry.updateAvailable && entry.compatible) {
            pendingSetup = entry.id
            Store.update(entry.id)
        } else {
            beginSetup(entry.id)
        }
    }

    function openLink() {
        linkEditing = true
        focusArea = "link"
        Qt.callLater(() => linkField.focusField())
    }

    function submitLink(text) {
        const url = String(text || "").trim()
        if (url.length === 0)
            return
        pendingSetup = "*"
        Store.addFromUrl(url)
        closeLink()
    }

    function closeLink() {
        linkEditing = false
        focusArea = "grid"
        InputKeys.focus(tiles)
    }

    function openMenu(index) {
        const entry = entries[index]
        if (!entry || !entry.installed || entry.removable === false || entry.bundled)
            return false
        menuEntry = entry
        menuLoader.active = true
        return true
    }

    function choose(index) {
        const entry = menuEntry
        closeMenu()
        if (index === 0)
            activateEntry(entries.findIndex(candidate => candidate.id === entry.id))
        else if (index === 1)
            removingEntry = entry
    }

    function closeMenu() {
        menuLoader.active = false
        InputKeys.focus(tiles)
    }

    function activate() {
        if (removeConfirmation.item)
            return removeConfirmation.item.activate()
        if (menuLoader.item)
            return menuLoader.item.activate()
        if (focusArea === "link") {
            if (!linkEditing)
                openLink()
            return
        }
        activateEntry(tiles.currentIndex)
    }

    function longPress() {
        return focusArea === "grid" && openMenu(tiles.currentIndex)
    }

    function back() {
        if (removingEntry) {
            removingEntry = null
            InputKeys.focus(tiles)
            return true
        }
        if (menuLoader.item) {
            closeMenu()
            return true
        }
        if (linkEditing) {
            closeLink()
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
        const columns = Math.max(1, tiles.columns)
        if (focusArea === "link") {
            if (key === Qt.Key_Up) {
                focusArea = "grid"
                InputKeys.focus(tiles)
            }
            return InputKeys.isDirection(key)
        }
        const index = tiles.currentIndex
        if (key === Qt.Key_Left && index % columns > 0)
            tiles.currentIndex = index - 1
        else if (key === Qt.Key_Right && index % columns < columns - 1 && index + 1 < entries.length)
            tiles.currentIndex = index + 1
        else if (key === Qt.Key_Up && index >= columns)
            tiles.currentIndex = index - columns
        else if (key === Qt.Key_Down) {
            if (index + columns < entries.length)
                tiles.currentIndex = index + columns
            else if (Math.floor(index / columns) < Math.floor((entries.length - 1) / columns))
                tiles.currentIndex = entries.length - 1
            else if (linkAvailable) {
                focusArea = "link"
                linkButton.forceActiveFocus()
            }
        } else if (key === Qt.Key_Menu || key === Qt.Key_M)
            return openMenu(index)
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
        function onProblem(message) {
            root.pendingSetup = ""
            root.problemText = message
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.implicitHeight + Metrics.pageMarginPx * 2
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        ColumnLayout {
            id: column
            x: (flick.width - width) / 2
            y: Math.max(Metrics.pageMarginPx, Metrics.scaled(56))
            width: Math.min(flick.width - Metrics.pageMarginPx * 2, Metrics.scaled(1120))
            spacing: Metrics.scaled(10)

            RowLayout {
                Layout.fillWidth: true
                spacing: Metrics.scaled(14)

                IconButton {
                    visible: !root.firstAccount
                    iconName: "arrow_back"
                    accessibleName: "Back"
                    onClicked: Router.pop("accounts")
                }

                AppText {
                    Layout.fillWidth: true
                    text: root.firstAccount ? "Welcome to Spool" : "Add an account"
                    font.pixelSize: Metrics.scaled(44)
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }

                BusySpinner {
                    Layout.preferredWidth: Metrics.scaled(24)
                    Layout.preferredHeight: Metrics.scaled(24)
                    running: Store.loading
                    visible: running
                }
            }

            AppText {
                Layout.fillWidth: true
                text: root.firstAccount ? "Where's your media? Choose the service it's on and sign in." :
                                          "Choose a service and sign in. You can add as many as you like."
                color: Theme.textSecondary
                font.pixelSize: Metrics.scaled(20)
                wrapMode: Text.WordWrap
            }
            AppText {
                Layout.fillWidth: true
                visible: Store.error.length > 0 && root.entries.length === 0
                text: Store.error
                color: Theme.errorText
                font.pixelSize: Metrics.bodySizePx
                wrapMode: Text.Wrap
            }

            GridView {
                id: tiles

                readonly property real minimumTile: Metrics.scaled(250)
                readonly property int columns: Math.max(1, Math.floor(width / minimumTile))

                Layout.fillWidth: true
                Layout.topMargin: Metrics.scaled(26)
                Layout.preferredHeight: Math.ceil(count / columns) * cellHeight
                interactive: false
                focus: true
                keyNavigationEnabled: false
                highlightMoveDuration: 0
                model: root.entries
                cellWidth: Math.floor(width / columns)
                cellHeight: Metrics.scaled(212)
                onCurrentIndexChanged: {
                    root.currentId = root.entries[currentIndex] ? String(root.entries[currentIndex].id || "") : ""
                    const item = currentItem
                    if (!item || !activeFocus)
                        return
                    const bounds = item.mapToItem(flick.contentItem, 0, 0, item.width, item.height)
                    if (bounds.y < flick.contentY)
                        flick.contentY = Math.max(0, bounds.y - Metrics.scaled(24))
                    else if (bounds.y + bounds.height > flick.contentY + flick.height)
                        flick.contentY = bounds.y + bounds.height + Metrics.scaled(24) - flick.height
                }

                delegate: Item {
                    id: slot

                    required property int index
                    required property var modelData
                    readonly property string serviceState: root.stateOf(modelData)
                    readonly property bool current: GridView.isCurrentItem && Metrics.keyboardFocusActive
                                                    && tiles.activeFocus && root.focusArea === "grid"

                    width: tiles.cellWidth
                    height: tiles.cellHeight

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: Metrics.scaled(8)
                        radius: Theme.radiusPanel
                        color: slot.current ? Theme.focusedFill : hover.hovered ? Theme.bgHover : Theme.bgRaised
                        border.width: slot.current ? Theme.focusBorderWidth : Theme.hoverBorderWidth
                        border.color: slot.current ? Theme.accent : Theme.border
                        opacity: slot.serviceState === "unavailable" ? 0.55 : 1

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: Metrics.scaled(20)
                            spacing: Metrics.scaled(8)

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Metrics.scaled(12)

                                ProviderIcon {
                                    Layout.preferredWidth: Metrics.scaled(64)
                                    Layout.preferredHeight: Metrics.scaled(64)
                                    source: slot.modelData.iconUrl || ""
                                    name: slot.modelData.name || ""
                                    seed: slot.modelData.id || ""
                                    // The folders provider is built in and has no package icon.
                                    symbol: slot.modelData.id === "spool.local" ? "computer" : ""
                                }
                                Item {
                                    Layout.fillWidth: true
                                }
                                BusySpinner {
                                    Layout.preferredWidth: Metrics.scaled(26)
                                    Layout.preferredHeight: Metrics.scaled(26)
                                    Layout.alignment: Qt.AlignTop
                                    running: slot.serviceState === "busy"
                                    visible: running
                                }
                                MaterialIcon {
                                    Layout.alignment: Qt.AlignTop
                                    visible: root.accountCount(slot.modelData) > 0 && slot.serviceState === "ready"
                                    name: "check_circle"
                                    iconSize: Metrics.scaled(24)
                                    iconColor: Theme.accent
                                }
                            }

                            AppText {
                                Layout.fillWidth: true
                                Layout.topMargin: Metrics.scaled(4)
                                text: slot.modelData.name || ""
                                font.pixelSize: Metrics.scaled(24)
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                                maximumLineCount: 1
                            }

                            SecondaryText {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                text: root.detailText(slot.modelData)
                                color: slot.serviceState === "unavailable" ? Theme.textMuted : Theme.textSecondary
                                font.pixelSize: Metrics.scaled(17)
                                wrapMode: Text.WordWrap
                                elide: Text.ElideRight
                                maximumLineCount: 2
                                verticalAlignment: Text.AlignTop
                            }

                            // Not made by Spool: said once, plainly, without
                            // turning the choice into a lecture.
                            AppText {
                                visible: slot.modelData.community === true
                                text: "Community"
                                color: Theme.textMuted
                                font.pixelSize: Metrics.scaled(14)
                                font.weight: Font.DemiBold
                                font.letterSpacing: Metrics.scaled(1)
                            }
                        }

                        HoverHandler {
                            id: hover
                        }

                        TapHandler {
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onTapped: (point, button) => {
                                tiles.currentIndex = slot.index
                                root.focusArea = "grid"
                                if (button === Qt.RightButton)
                                    root.openMenu(slot.index)
                                else
                                    root.activateEntry(slot.index)
                            }
                            onLongPressed: root.openMenu(slot.index)
                        }
                    }
                }
            }

            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: Metrics.scaled(12)
                visible: root.pendingSetup === "*" || root.problemText.length > 0
                spacing: Metrics.scaled(10)
                BusySpinner {
                    Layout.preferredWidth: Metrics.scaled(20)
                    Layout.preferredHeight: Metrics.scaled(20)
                    running: root.pendingSetup === "*"
                    visible: running
                }
                AppText {
                    text: root.pendingSetup === "*" ? "Getting the provider from that link…" : root.problemText
                    color: root.pendingSetup === "*" ? Theme.textSecondary : Theme.errorText
                    font.pixelSize: Metrics.bodySizePx
                    wrapMode: Text.Wrap
                }
            }

            ActionButton {
                id: linkButton
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: Metrics.scaled(18)
                visible: root.linkAvailable && !root.linkEditing
                kind: "flat"
                iconName: "link"
                text: "Have a link to a provider?"
                onClicked: root.openLink()
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.maximumWidth: Metrics.scaled(720)
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: Metrics.scaled(18)
                visible: root.linkEditing
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
                        onClicked: root.closeLink()
                    }
                    ActionButton {
                        text: "Add"
                        kind: "primary"
                        enabled: linkField.text.trim().length > 0
                        onClicked: root.submitLink(linkField.text)
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
            anchorItem: tiles.currentItem
            title: root.menuEntry ? root.menuEntry.name : ""
            options: ["Add an account", "Remove from this device"]
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
            title: "Remove " + root.removingEntry.name + "?"
            message: "Its accounts are signed out on this device. Nothing on your server is deleted, and you can add it again any time."
            confirmText: "Remove"
            destructive: true
            onAccepted: {
                Store.uninstall(root.removingEntry.id)
                root.removingEntry = null
                InputKeys.focus(tiles)
            }
            onDismissed: {
                root.removingEntry = null
                InputKeys.focus(tiles)
            }
        }
    }
}
