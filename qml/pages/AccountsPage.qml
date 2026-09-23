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
    readonly property var accounts: Providers.accounts
    readonly property int count: accounts.length + 1
    readonly property int tileSize: Metrics.scaled(Metrics.laneAtLeast(width, "wide") ? 148 : Metrics.laneAtLeast(width, "regular") ? 132 :
                                                                                                                                      104)
    property var menuAccount: null

    focus: true

    function accountAt(index) {
        return index < accounts.length ? accounts[index] : null
    }

    function activateAt(index) {
        const account = accountAt(index)
        if (!account) {
            shell.pushRoute("addProvider")
            return
        }
        if (account.needsSignIn) {
            shell.openProviderScreen(Providers.beginSetup(account.moduleId))
            return
        }
        Providers.useAccount(account.id)
        shell.goHome()
    }

    function openMenu(index) {
        const account = accountAt(index)
        if (!account)
            return false
        menuAccount = account
        menuLoader.active = true
        return true
    }

    function activate() {
        if (menuLoader.item)
            return menuLoader.item.activate()
        activateAt(grid.currentIndex)
    }

    function longPress() {
        return openMenu(grid.currentIndex)
    }

    function back() {
        if (!menuLoader.item)
            return false
        menuLoader.active = false
        InputKeys.focus(root)
        return true
    }

    function routeKey(key, phase, repeat) {
        if (menuLoader.item)
            return menuLoader.item.routeKey(key, phase, repeat)
        if (phase !== "press")
            return InputKeys.isDirection(key)
        const columns = Math.max(1, Math.floor(grid.width / grid.cellWidth))
        if (key === Qt.Key_Left)
            grid.currentIndex = Math.max(0, grid.currentIndex - 1)
        else if (key === Qt.Key_Right)
            grid.currentIndex = Math.min(count - 1, grid.currentIndex + 1)
        else if (key === Qt.Key_Down)
            grid.currentIndex = Math.min(count - 1, grid.currentIndex + columns)
        else if (key === Qt.Key_Up) {
            if (grid.currentIndex < columns && shell)
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
            text: "Who's watching?"
            font.pixelSize: Metrics.titleSizePx
            font.weight: Font.DemiBold
        }

        GridView {
            id: grid
            Layout.alignment: Qt.AlignHCenter
            Layout.fillHeight: true
            readonly property int columns: Math.max(1, Math.min(root.count, Math.floor(parent.width / cellWidth)))
            Layout.preferredWidth: columns * cellWidth
            cellWidth: root.tileSize + Metrics.scaled(28)
            cellHeight: root.tileSize + Metrics.scaled(82)
            model: root.count
            interactive: contentHeight > height
            boundsBehavior: Flickable.StopAtBounds
            currentIndex: 0
            focus: true

            delegate: Item {
                id: cell
                required property int index
                readonly property var account: root.accountAt(index)
                width: grid.cellWidth
                height: grid.cellHeight

                ProfileTile {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.topMargin: Metrics.scaled(8)
                    tileSize: root.tileSize
                    focused: cell.GridView.isCurrentItem && Metrics.keyboardFocusActive
                    addTile: !cell.account
                    username: cell.account ? cell.account.label : "Add"
                    serverName: cell.account ? cell.account.detail : ""
                    needsSignIn: Boolean(cell.account && cell.account.needsSignIn)
                    opacity: !cell.account || cell.account.enabled ? 1 : 0.55
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

                Rectangle {
                    visible: Boolean(cell.account) && cell.account.enabled && !cell.account.running
                    x: parent.width / 2 - root.tileSize / 2 + Metrics.scaled(8)
                    y: Metrics.scaled(16)
                    width: Metrics.scaled(10)
                    height: width
                    radius: width / 2
                    color: Theme.pending
                }
            }
        }
    }

    readonly property var menuActions: {
        const account = menuAccount
        if (!account)
            return []
        const out = []
        if (account.hasSettings)
            out.push({
                         "label": "Account settings",
                         "value": "settings"
                     })
        out.push({
                     "label": account.enabled ? "Hide from home" : "Show on home",
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
        if (action === "settings")
            shell.openProviderScreen(Providers.openSettings(account.id))
        else if (action === "toggle")
            Providers.setAccountEnabled(account.id, !account.enabled)
        else if (action === "remove")
            Providers.removeAccount(account.id)
    }

    Loader {
        id: menuLoader
        anchors.fill: parent
        active: false
        sourceComponent: OptionPickerDialog {
            visible: true
            anchorItem: grid.currentItem
            title: root.menuAccount ? root.menuAccount.label : ""
            options: root.menuActions.map(action => action.label)
            currentIndex: 0
            onSelected: index => root.choose(index)
            onDismissed: {
                menuLoader.active = false
                InputKeys.focus(root)
            }
        }
    }
}
