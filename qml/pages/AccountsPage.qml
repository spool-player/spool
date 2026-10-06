pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

// Every account on every provider. Choosing one makes it the one in use for
// its server (other people on that server step aside); accounts on different
// servers and providers stay on home together. The last tile adds a provider.
FocusScope {
    id: root

    property var shell
    property string serverFilter: ""
    readonly property var serverGroups: {
        const groups = []
        for (const account of Providers.accounts) {
            const key = JSON.stringify([account.moduleId, account.group || account.id])
            if (!groups.some(group => group.key === key))
                groups.push({
                                key: key,
                                label: String(account.detail || account.address || account.providerName),
                                provider: String(account.providerName)
                            })
        }
        return groups
    }
    readonly property var accounts: Providers.accounts.filter(account => !serverFilter || JSON.stringify([account.moduleId,
                                                                                                          account.group
                                                                                                          || account.id])
                                                                         === serverFilter).slice().sort((a, b) => (
                                                                         a.moduleId + "/" + (a.group
                                                                                             || a.id)).localeCompare(
                                        b.moduleId + "/" + (b.group || b.id)))
    readonly property int count: accounts.length + 1
    readonly property int tileSize: Metrics.scaled(Metrics.laneAtLeast(width, "wide") ? 148 : Metrics.laneAtLeast(width, "regular") ? 132 :
                                                                                                                                      104)
    property var menuAccount: null
    property var removingAccount: null
    property string pendingAccountId: ""
    property string selectionMessage: ""
    property bool choosingServer: false
    readonly property bool selecting: pendingAccountId.length > 0
    readonly property var selectedAccount: accountAt(grid.currentIndex)

    Connections {
        target: Providers
        function onAccountSelectionFinished(accountId, selected) {
            if (accountId !== root.pendingAccountId)
                return
            root.pendingAccountId = ""
            if (selected)
                root.shell.goHome()
            else {
                const account = Providers.accounts.find(account => account.id === accountId)
                root.selectionMessage = account && account.errorText ? account.errorText :
                                                                       "Profile switch cancelled. Your current profile is unchanged."
            }
        }
    }

    function openServerMenu() {
        choosingServer = true
        menuLoader.active = true
    }

    focus: true

    function accountAt(index) {
        return index < accounts.length ? accounts[index] : null
    }

    function activateAt(index) {
        if (selecting)
            return
        selectionMessage = ""
        const account = accountAt(index)
        if (!account) {
            shell.pushRoute("addProvider")
            return
        }
        if (account.needsSignIn) {
            shell.openProviderScreen(Providers.beginSetup(account.moduleId))
            return
        }
        pendingAccountId = account.id
        Providers.useAccount(account.id)
    }

    function openMenu(index) {
        choosingServer = false
        const account = accountAt(index)
        if (!account)
            return false
        menuAccount = account
        menuLoader.active = true
        return true
    }

    function activate() {
        if (removeConfirmation.item)
            return removeConfirmation.item.activate()
        if (menuLoader.item)
            return menuLoader.item.activate()
        if (serverButton.activeFocus) {
            openServerMenu()
            return
        }
        if (addProfileButton.activeFocus) {
            if (addProfileButton.enabled && selectedAccount)
                shell.openProviderScreen(Providers.beginSetup(selectedAccount.moduleId))
            return
        }
        if (optionsButton.activeFocus) {
            openMenu(grid.currentIndex)
            return
        }
        activateAt(grid.currentIndex)
    }

    function longPress() {
        return !serverButton.activeFocus && !addProfileButton.activeFocus && !optionsButton.activeFocus && openMenu(
                    grid.currentIndex)
    }

    function back() {
        if (removingAccount) {
            removingAccount = null
            InputKeys.focus(grid)
            return true
        }
        if (!menuLoader.item)
            return false
        menuLoader.active = false
        InputKeys.focus(root)
        return true
    }

    function routeKey(key, phase, repeat) {
        if (removeConfirmation.item)
            return removeConfirmation.item.routeKey(key, phase, repeat)
        if (menuLoader.item)
            return menuLoader.item.routeKey(key, phase, repeat)
        if (phase !== "press")
            return InputKeys.isDirection(key)
        const buttons = [serverButton, addProfileButton, optionsButton].filter(button => button.visible
                                                                                         && button.enabled)

        const focusedButton = buttons.findIndex(button => button.activeFocus)
        if (focusedButton >= 0) {
            if (key === Qt.Key_Down) {
                InputKeys.focus(focusedButton + 1 < buttons.length ? buttons[focusedButton + 1] : grid)
                return true
            }
            if (key === Qt.Key_Up) {
                if (focusedButton > 0)
                    InputKeys.focus(buttons[focusedButton - 1])
                else if (shell)
                    shell.focusNavBar()
                return true
            }
            return InputKeys.isDirection(key)
        }
        const columns = Math.max(1, Math.floor(grid.width / grid.cellWidth))
        if (key === Qt.Key_Left)
            grid.currentIndex = Math.max(0, grid.currentIndex - 1)
        else if (key === Qt.Key_Right)
            grid.currentIndex = Math.min(count - 1, grid.currentIndex + 1)
        else if (key === Qt.Key_Down)
            grid.currentIndex = Math.min(count - 1, grid.currentIndex + columns)
        else if (key === Qt.Key_Up) {
            if (grid.currentIndex < columns && buttons.length)
                InputKeys.focus(buttons[buttons.length - 1])
            else if (grid.currentIndex < columns && shell)
                shell.focusNavBar()
            else
                grid.currentIndex = Math.max(0, grid.currentIndex - columns)
        } else if (key === Qt.Key_Menu || key === Qt.Key_M)
            return openMenu(grid.currentIndex)
        else
            return false
        return true
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Metrics.pageMarginPx
        spacing: Metrics.scaled(24)

        AppText {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: Metrics.scaled(12)
            text: "Profiles & servers"
            font.pixelSize: Metrics.titleSizePx
            font.weight: Font.DemiBold
        }

        AppText {
            Layout.fillWidth: true
            text: "Choose who's watching on a server. Independent servers appear together on Home."
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
        }

        ActionButton {
            id: serverButton
            Layout.alignment: Qt.AlignHCenter
            visible: root.serverGroups.length > 1
            text: {
                const group = root.serverGroups.find(group => group.key === root.serverFilter)
                return group ? group.label + " · " + group.provider : "All servers"
            }
            kind: "secondary"
            onClicked: root.openServerMenu()
        }

        ActionButton {
            id: addProfileButton
            Layout.alignment: Qt.AlignHCenter
            visible: Boolean(root.selectedAccount)
            enabled: !root.selecting
            text: "Add another watching profile"
            kind: "secondary"
            onClicked: root.shell.openProviderScreen(Providers.beginSetup(root.selectedAccount.moduleId))
        }
        ActionButton {
            id: optionsButton
            Layout.alignment: Qt.AlignHCenter
            visible: Boolean(root.selectedAccount)
            enabled: !root.selecting
            text: "Account options"
            kind: "secondary"
            onClicked: root.openMenu(grid.currentIndex)
        }

        SecondaryText {
            Layout.fillWidth: true
            visible: Boolean(root.selectedAccount && root.selectedAccount.errorText)
            text: root.selectedAccount ? String(root.selectedAccount.errorText || "") : ""
            font.pixelSize: Metrics.bodySizePx
            color: Theme.errorText
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }

        AppText {
            Layout.fillWidth: true
            visible: root.selecting || root.selectionMessage.length > 0
            text: root.selecting ? "Switching profile… Complete any sign-in or PIN request to continue." :
                                   root.selectionMessage
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
        }

        AppText {
            readonly property var selectedAccount: root.accountAt(grid.currentIndex)
            Layout.fillWidth: true
            visible: Boolean(selectedAccount && selectedAccount.missingHostExtensions
                             && selectedAccount.missingHostExtensions.length > 0)
            text: "Update Spool to use all features of this provider."
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            font.pixelSize: Metrics.bodySizePx
            color: Theme.textSecondary
        }

        GridView {
            id: grid
            Layout.alignment: Qt.AlignHCenter
            Layout.fillHeight: true
            readonly property int columns: Math.max(1, Math.min(root.count, Math.floor(parent.width / cellWidth)))
            Layout.preferredWidth: columns * cellWidth
            cellWidth: root.tileSize + Metrics.scaled(28)
            cellHeight: root.tileSize + Metrics.scaled(104)
            model: root.count
            interactive: contentHeight > height
            boundsBehavior: Flickable.StopAtBounds
            currentIndex: 0
            focus: true

            delegate: Item {
                id: cell
                required property int index
                readonly property var account: root.accountAt(index)
                readonly property string connectionState: account ? String(account.connectionState || "starting") : ""
                readonly property string stateLabel: !account ? "" : account.needsSignIn ? "Sign in to reconnect" :
                                                                                           !account.enabled &&
                                                                                           !account.pendingEnabled
                                                                                           ? "Switch profile" :
                                                                                             connectionState
                                                                                             === "starting" && (
                                                                                                 account.enabled
                                                                                                 || account.pendingEnabled)
                                                                                             ? "Connecting…" :
                                                                                               connectionState
                                                                                               === "failed"
                                                                                               ? "Unavailable · select to retry" :
                                                                                                 account.enabled
                                                                                                 && connectionState
                                                                                                 === "active"
                                                                                                 ? "Watching" :
                                                                                                   connectionState
                                                                                                   === "locked"
                                                                                                   ? "Select profile / unlock" :
                                                                                                     "Switch profile"
                width: grid.cellWidth
                height: grid.cellHeight

                ProfileTile {
                    id: profileTile
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.topMargin: Metrics.scaled(8)
                    tileSize: root.tileSize
                    focused: cell.GridView.isCurrentItem && Metrics.keyboardFocusActive
                    addTile: !cell.account
                    username: cell.account ? cell.account.label : "Add"
                    serverName: cell.account ? String(cell.account.detail || cell.account.address
                                                      || cell.account.providerName) : ""
                    needsSignIn: Boolean(cell.account && cell.account.needsSignIn)
                    opacity: !cell.account || cell.account.enabled || cell.account.pendingEnabled ? 1 : 0.75
                    onAccepted: {
                        grid.currentIndex = cell.index
                        root.activateAt(cell.index)
                    }
                    onContextRequested: {
                        grid.currentIndex = cell.index
                        root.openMenu(cell.index)
                    }
                }

                // Which provider an account is on, at a glance.
                ProviderIcon {
                    visible: Boolean(cell.account)
                    x: parent.width / 2 + root.tileSize / 2 - width + Metrics.scaled(6)
                    y: Metrics.scaled(8) + root.tileSize - height + Metrics.scaled(6)
                    width: Metrics.scaled(34)
                    height: width
                    border.width: Metrics.scaled(3)
                    border.color: Theme.bg
                    source: cell.account ? cell.account.iconUrl : ""
                    name: cell.account ? cell.account.providerName : ""
                    seed: cell.account ? cell.account.moduleId : ""
                }

                MaterialIcon {
                    visible: Boolean(cell.account) && cell.account.enabled && cell.connectionState !== "active"
                    x: parent.width / 2 - root.tileSize / 2 + Metrics.scaled(8)
                    y: Metrics.scaled(16)
                    iconSize: Metrics.scaled(18)
                    name: cell.connectionState === "locked" ? "lock" : cell.connectionState === "failed"
                                                              ? "error_outline" : "hourglass_empty"
                    iconColor: cell.connectionState === "failed" ? Theme.errorText : Theme.pending
                }
                AppText {
                    anchors.top: profileTile.bottom
                    anchors.topMargin: Metrics.scaled(4)
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: root.tileSize
                    visible: cell.stateLabel.length > 0
                    text: cell.stateLabel
                    horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: Metrics.bodySizePx
                    color: cell.connectionState === "failed" ? Theme.errorText : Theme.textSecondary
                    elide: Text.ElideRight
                }
            }
        }
    }

    readonly property var menuActions: {
        const account = menuAccount
        if (!account)
            return []
        const out = []
        if (account.needsSignIn || account.connectionState === "failed")
            out.push({
                         "label": "Sign in / reconnect",
                         "value": "reconnect"
                     })
        if (account.hasSettings)
            out.push({
                         "label": "Account settings",
                         "value": "settings"
                     })
        out.push({
                     "label": account.enabled || account.pendingEnabled ? "Hide from home" : "Show on home",
                     "value": "toggle"
                 })
        out.push({
                     "label": "Remove",
                     "value": "remove"
                 })
        return out
    }

    function choose(index) {
        const account = menuAccount
        const action = menuActions[index] ? menuActions[index].value : ""
        menuLoader.active = false
        InputKeys.focus(root)
        if (action === "reconnect")
            shell.openProviderScreen(Providers.beginSetup(account.moduleId))
        else if (action === "settings")
            shell.openProviderScreen(Providers.openSettings(account.id))
        else if (action === "toggle")
            Providers.setAccountEnabled(account.id, !(account.enabled || account.pendingEnabled))
        else if (action === "remove")
            removingAccount = account
    }

    Loader {
        id: menuLoader
        anchors.fill: parent
        active: false
        sourceComponent: OptionPickerDialog {
            visible: true
            anchorItem: grid.currentItem
            title: root.choosingServer ? "Choose server" : root.menuAccount ? root.menuAccount.label : ""
            options: root.choosingServer ? ["All servers"].concat(root.serverGroups.map(group => group.label + " · "
                                                                                                 + group.provider)) :
                                           root.menuActions.map(action => action.label)
            currentIndex: root.choosingServer ? Math.max(0, root.serverGroups.findIndex(group => group.key
                                                                                                 === root.serverFilter)
                                                         + 1) : 0
            onSelected: index => {
                if (root.choosingServer) {
                    root.serverFilter = index > 0 ? root.serverGroups[index - 1].key : ""
                    grid.currentIndex = 0
                    menuLoader.active = false
                    InputKeys.focus(grid)
                } else
                    root.choose(index)
            }
            onDismissed: {
                menuLoader.active = false
                InputKeys.focus(root)
            }
        }
    }

    Loader {
        id: removeConfirmation
        anchors.fill: parent
        active: Boolean(root.removingAccount)
        z: 210
        sourceComponent: ConfirmationDialog {
            title: "Remove account?"
            message: "Remove " + root.removingAccount.label
                     + " from Spool on this device? Your server account and media will not be deleted." + (
                         typeof SettingsSync !== "undefined" && root.removingAccount.id === SettingsSync.accountId
                         ? " This is your settings sync account. Sync will pause until you choose another account." :
                           "")
            confirmText: "Remove account"
            destructive: true
            onAccepted: {
                Providers.removeAccount(root.removingAccount.id)
                root.removingAccount = null
                InputKeys.focus(grid)
            }
            onDismissed: {
                root.removingAccount = null
                InputKeys.focus(grid)
            }
        }
    }
}
