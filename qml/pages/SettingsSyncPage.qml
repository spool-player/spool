pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

FocusScope {
    id: root
    property var shell
    property var syncController: SettingsSync
    property var settingsController: Settings
    property var accountController: Providers
    readonly property bool contentReady: true
    property bool choosingAccount: false
    property bool advancedExpanded: false
    readonly property var account: syncController.accounts.find(candidate => candidate.id === syncController.accountId)
                                   || null
    readonly property var accounts: syncController.accounts.filter(candidate => candidate.syncSupported)
    readonly property string accountLabel: account ? String(account.syncLabel) : syncController.accountId.length ? "Account removed" :
                                                                                                                   "Choose an account"
    readonly property string status: syncController.status
    readonly property string recoveryLabel: ({
                                                 off: "Turn on sync",
                                                 noAccount: accounts.length ? "Choose account" : "Add account",
                                                 removed: accounts.length ? "Choose account" : "Add account",
                                                 signedOut: "Sign in again",
                                                 inactive: "Switch to this profile",
                                                 failed: "Reconnect",
                                                 unsupported: "Choose account",
                                                 offline: "Retry now",
                                                 error: "Retry now",
                                                 saveFailed: "Retry saving"
                                             })[status] || ""
    readonly property var categories: {
        const groups = []
        for (const spec of settingsController.settingsSchema) {
            const state = syncController.states[spec.key]
            if (!state || !state.eligible)
                continue
            const device = spec.syncPolicy === "device" || spec.key === "subtitles/font" && String(
                      settingsController.values[spec.key]).startsWith("system:")
            const name = device ? "Device-specific" : spec.group === "Appearance" ? "Interface" : spec.group
            let group = groups.find(candidate => candidate.title === name)
            if (!group) {
                group = {
                    title: name,
                    keys: [],
                    enabled: 0,
                    device: device
                }
                groups.push(group)
            }
            group.keys.push(spec.key)
            if (state.enabled)
                ++group.enabled
        }
        return groups
    }
    readonly property var rows: {
        const result = ["enabled", "account", "preset"]
        if (advancedExpanded && account)
            for (let index = 0; index < categories.length; ++index)
                result.push("category/" + index)
        result.push("syncNow")
        return result
    }

    function focusEntry() {
        InputKeys.focus(recovery.visible ? recovery : list)
    }
    function openAccountPicker() {
        if (accounts.length)
            choosingAccount = true
        else if (shell)
            shell.pushRoute("addProvider")
    }
    function recover() {
        if (status === "off")
            syncController.setEnabled(true)
        else if (status === "signedOut" && account && shell)
            shell.openProviderScreen(accountController.beginSetup(account.moduleId))
        else if ((status === "inactive" || status === "failed") && account)
            accountController.useAccount(account.id)
        else if (status === "offline" || status === "error" || status === "saveFailed")
            syncController.retry()
        else
            openAccountPicker()
    }
    function activateRow(index) {
        const key = rows[index]
        if (key === "enabled")
            syncController.setEnabled(!syncController.enabled)
        else if (key === "account")
            openAccountPicker()
        else if (key === "preset") {
            advancedExpanded = !advancedExpanded
            list.currentIndex = index
        } else if (key === "syncNow" && syncController.enabled && status !== "syncing" && account
                   && account.connectionState === "active")
            syncController.retry()
        else if (key && key.startsWith("category/")) {
            const category = categories[Number(key.substring(9))]
            syncController.setSettingsEnabled(category.keys, category.enabled !== category.keys.length)
        }
    }
    function back() {
        if (syncController.accountChangePending) {
            syncController.confirmAccountChange(false)
            InputKeys.focus(list)
            return true
        }
        if (choosingAccount) {
            choosingAccount = false
            InputKeys.focus(list)
            return true
        }
        if (advancedExpanded) {
            advancedExpanded = false
            list.currentIndex = 2
            InputKeys.focus(list)
            return true
        }
        return false
    }
    function activate() {
        if (confirmation.item)
            confirmation.item.activate()
        else if (picker.item)
            picker.item.activate()
        else if (recovery.activeFocus)
            recover()
        else if (chooseAnother.activeFocus)
            openAccountPicker()
        else if (recommended.activeFocus)
            syncController.resetSettingOverrides()
        else
            activateRow(list.currentIndex)
    }
    function routeKey(key, phase, repeat) {
        if (confirmation.item)
            return confirmation.item.routeKey(key, phase, repeat)
        if (picker.item)
            return picker.item.routeKey(key, phase, repeat)
        if (phase !== "press")
            return InputKeys.isDirection(key)
        const buttons = [recovery, chooseAnother, recommended].filter(button => button.visible)
        const focusedButton = buttons.findIndex(button => button.activeFocus)
        if (focusedButton >= 0) {
            if (key === Qt.Key_Down)
                InputKeys.focus(list)
            else if (key === Qt.Key_Up && shell)
                shell.focusNavBar()
            else if (key === Qt.Key_Right)
                InputKeys.focus(buttons[Math.min(buttons.length - 1, focusedButton + 1)])
            else if (key === Qt.Key_Left)
                InputKeys.focus(buttons[Math.max(0, focusedButton - 1)])
            return InputKeys.isDirection(key)
        }
        if (key === Qt.Key_Up && list.currentIndex === 0) {
            if (recovery.visible || recommended.visible)
                InputKeys.focus(recovery.visible ? recovery : recommended)
            else if (shell)
                shell.focusNavBar()
            return true
        }
        if (key === Qt.Key_Up || key === Qt.Key_Down)
            return list.moveSelection(key === Qt.Key_Up ? -1 : 1)
        return InputKeys.isDirection(key)
    }
    focus: true
    onActiveFocusChanged: if (activeFocus)
                              Qt.callLater(focusEntry)
    Component.onCompleted: if (activeFocus)
                               Qt.callLater(focusEntry)

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Metrics.pageMarginPx
        spacing: Metrics.scaled(16)
        AppText {
            Layout.fillWidth: true
            text: "Settings sync"
            font.pixelSize: Metrics.titleSizePx
            font.weight: Font.DemiBold
        }
        Surface {
            Layout.fillWidth: true
            implicitHeight: statusContent.implicitHeight + Metrics.scaled(32)
            ColumnLayout {
                id: statusContent
                anchors.fill: parent
                anchors.margins: Metrics.scaled(16)
                spacing: Metrics.scaled(10)
                AppText {
                    objectName: "syncSummary"
                    Layout.fillWidth: true
                    text: root.syncController.summary
                    font.pixelSize: Metrics.bodySizePx
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
                SecondaryText {
                    Layout.fillWidth: true
                    visible: Boolean(root.account)
                    text: root.accountLabel
                    font.pixelSize: Metrics.bodySizePx
                    wrapMode: Text.Wrap
                }
                SecondaryText {
                    Layout.fillWidth: true
                    visible: root.status === "inactive"
                    text: "Switching resumes sync and changes who is watching on this server. You can choose a different sync account instead."
                    font.pixelSize: Metrics.bodySizePx
                    wrapMode: Text.Wrap
                }
                Flow {
                    Layout.fillWidth: true
                    Layout.preferredHeight: implicitHeight
                    visible: root.recoveryLabel.length > 0 || root.syncController.customized
                    spacing: Metrics.scaled(12)
                    ActionButton {
                        id: recovery
                        objectName: "syncRecovery"
                        width: Math.min(implicitWidth, parent.width)
                        text: root.recoveryLabel
                        visible: text.length > 0
                        kind: "primary"
                        onClicked: root.recover()
                    }
                    ActionButton {
                        id: chooseAnother
                        width: Math.min(implicitWidth, parent.width)
                        text: "Choose another account"
                        kind: "secondary"
                        visible: Boolean(root.account) && root.accounts.length > 1 && root.recoveryLabel.length > 0
                        onClicked: root.openAccountPicker()
                    }
                    ActionButton {
                        id: recommended
                        width: Math.min(implicitWidth, parent.width)
                        objectName: "syncRecommended"
                        text: "Use recommended settings"
                        kind: "secondary"
                        visible: root.syncController.customized
                        onClicked: root.syncController.resetSettingOverrides()
                    }
                }
                SecondaryText {
                    Layout.fillWidth: true
                    visible: root.advancedExpanded && root.syncController.problemDetail.length > 0
                    text: root.syncController.problemDetail
                    font.pixelSize: Metrics.bodySizePx
                    wrapMode: Text.Wrap
                }
            }
        }
        MenuListView {
            id: list
            objectName: "syncSettingsList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: root.rows
            dismissOnBack: false
            dismissOnHorizontal: false
            spacing: Metrics.scaled(10)
            onAccepted: index => root.activateRow(index)
            delegate: Loader {
                id: rowLoader
                required property int index
                required property string modelData
                readonly property bool rowCurrent: ListView.isCurrentItem && list.activeFocus
                readonly property var category: modelData.startsWith("category/") ? root.categories[Number(modelData.substring(
                                                                                                               9))] : null
                width: list.width
                sourceComponent: modelData === "enabled" || category ? toggleRow : settingRow
                Component {
                    id: toggleRow
                    ToggleRow {
                        width: rowLoader.width
                        focus: false
                        focusPolicy: Qt.NoFocus
                        rowFocus: rowLoader.rowCurrent
                        title: rowLoader.category ? rowLoader.category.title : "Sync settings"
                        description: rowLoader.category ? rowLoader.category.device
                                                          ? "Usually differs between devices. Off in the recommended preset." :
                                                            rowLoader.category.enabled > 0
                                                            && rowLoader.category.enabled
                                                            < rowLoader.category.keys.length
                                                            ? "Some settings included. Turn on to include this whole category." :
                                                              "Include this category between devices" :
                                                              "Uses one account, independent of playback"
                        checked: rowLoader.category ? rowLoader.category.enabled === rowLoader.category.keys.length :
                                                      root.syncController.enabled
                        onToggled: root.activateRow(rowLoader.index)
                    }
                }
                Component {
                    id: settingRow
                    SettingRow {
                        width: rowLoader.width
                        focus: false
                        focusPolicy: Qt.NoFocus
                        rowFocus: rowLoader.rowCurrent
                        title: rowLoader.modelData === "account" ? "Account" : rowLoader.modelData === "preset"
                                                                   ? "What syncs" : "Sync now"
                        description: rowLoader.modelData === "preset"
                                     ? "Recommended: language, look, audio and subtitle preferences. Device-specific settings stay local." :
                                       rowLoader.modelData === "account" ? root.accountLabel :
                                                                           "Changes are saved locally even while sync is paused"
                        valueText: rowLoader.modelData === "account" ? "Choose" : rowLoader.modelData === "preset"
                                                                       ? root.advancedExpanded ? "Less" :
                                                                                                 root.syncController.customized
                                                                                                 ? "Custom · manage" :
                                                                                                   "Recommended · customize" :
                                                                                                   root.status
                                                                                                   === "syncing"
                                                                                                   ? "Syncing…" :
                                                                                                     root.status
                                                                                                     === "synced"
                                                                                                     ? "Up to date" :
                                                                                                       "Sync"
                        enabled: rowLoader.modelData !== "syncNow" || root.syncController.enabled && root.status !== "syncing"
                                 && Boolean(root.account && root.account.connectionState === "active")
                        onClicked: root.activateRow(rowLoader.index)
                    }
                }
            }
        }
    }
    Loader {
        id: picker
        anchors.fill: parent
        active: root.choosingAccount
        z: 200
        sourceComponent: OptionPickerDialog {
            visible: true
            title: "Choose sync account"
            options: root.accounts.map(account => String(account.syncLabel) + (account.connectionState === "active"
                                                                               ? "" : account.needsSignIn
                                                                                 ? " · sign in needed" :
                                                                                   " · not active"))
            currentIndex: Math.max(0, root.accounts.findIndex(account => account.id === root.syncController.accountId))
            onSelected: index => {
                root.choosingAccount = false
                root.syncController.setAccountId(String(root.accounts[index].id))
                InputKeys.focus(list)
            }
            onDismissed: {
                root.choosingAccount = false
                InputKeys.focus(list)
            }
        }
    }
    Loader {
        id: confirmation
        anchors.fill: parent
        active: root.syncController.accountChangePending
        z: 210
        sourceComponent: ConfirmationDialog {
            title: "Change settings sync account?"
            message: root.syncController.accountChangeWarning
            confirmText: "Change account"
            onAccepted: {
                root.syncController.confirmAccountChange(true)
                InputKeys.focus(list)
            }
            onDismissed: {
                root.syncController.confirmAccountChange(false)
                InputKeys.focus(list)
            }
        }
    }
}
