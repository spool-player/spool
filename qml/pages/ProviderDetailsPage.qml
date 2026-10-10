pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

// Host-owned package information; account configuration remains provider-owned.
FocusScope {
    id: root
    property var shell
    readonly property string moduleId: shell ? String(shell.routeArgs.moduleId || "") : ""
    readonly property var installedModule: Providers.modules.find(module => module.id === moduleId) || null
    readonly property var accounts: Providers.accounts.filter(account => account.moduleId === moduleId)
    readonly property var provenanceEntry: Store.installedProviders.find(entry => entry.id === moduleId) || null
    readonly property string provenance: provenanceEntry ? provenanceEntry.provenance : "unknown"
    readonly property var updateEntry: Store.updates.find(entry => entry.id === moduleId) || Store.official.concat(
                                           Store.community).find(entry => entry.id === moduleId
                                                                          && entry.updateAvailable) || null
    readonly property bool busy: Boolean(Store.busy[moduleId])
    readonly property string provenanceText: ({
                                                  native: "Built into Spool",
                                                  bundled: "Bundled with Spool",
                                                  official: "Spool provider store",
                                                  community: "Community provider store",
                                                  url: "Installed from a provider link",
                                                  file: "Installed from a local file (unverified publisher)",
                                                  unknown: "Installation source not recorded"
                                              })[provenance] || "Installation source not recorded"
    property string lastFocus: ""
    property var removingAccount: null
    property bool removingModule: false
    property string notice: ""
    readonly property bool modalVisible: confirmation.active
    focus: true

    function controls() {
        const items = [backButton, updateButton, addButton, removeButton].filter(item => item.visible && item.enabled)
        for (let i = 0; i < accountRepeater.count; ++i) {
            const row = accountRepeater.itemAt(i)
            if (row)
                items.push(...row.controls())
        }
        return items
    }
    function candidate(items) {
        return InputKeys.topLeftVisibleCandidate({
                                                     count: items.length,
                                                     width: flick.width,
                                                     height: flick.height,
                                                     itemAtIndex: index => items[index],
                                                     mapToItem: (item, x, y, width, height) => flick.mapToItem(item, x,
                                                                                                               y, width, height)
                                                 }, flick)
    }
    function usable(item) {
        if (!item || !item.visible || !item.enabled)
            return false
        const visible = candidate([item])
        return visible && (visible.fullyVisible || visible.visibleFraction >= InputKeys.focusRecoveryVisibleThreshold)
    }
    function recover() {
        const items = controls()
        const visible = candidate(items)
        if (!visible)
            return false
        items[visible.index].forceActiveFocus()
        lastFocus = items[visible.index].focusKey
        return true
    }
    function focusControl(item) {
        if (!item)
            return
        item.forceActiveFocus()
        lastFocus = item.focusKey
        const bounds = item.mapToItem(flick.contentItem, 0, 0, item.width, item.height)
        const margin = Metrics.scaled(12)
        if (bounds.y < flick.contentY + margin)
            flick.contentY = Math.max(0, bounds.y - margin)
        else if (bounds.y + bounds.height > flick.contentY + flick.height - margin)
            flick.contentY = Math.min(Math.max(0, flick.contentHeight - flick.height), bounds.y + bounds.height + margin
                                      - flick.height)
    }
    function restoreFocus() {
        if (!activeFocus || confirmation.active || controls().some(item => item.activeFocus))
            return
        const item = controls().find(item => item.focusKey === lastFocus)
        if (usable(item))
            item.forceActiveFocus()
        else
            recover()
    }
    onAccountsChanged: Qt.callLater(restoreFocus)
    onInstalledModuleChanged: Qt.callLater(restoreFocus)
    function routeKey(key, phase, repeat) {
        if (confirmation.item)
            return confirmation.item.routeKey(key, phase, repeat)
        if (!InputKeys.isDirection(key))
            return false
        if (phase !== "press")
            return true
        const items = controls()
        const index = items.findIndex(item => item.activeFocus)
        if (index < 0 || !usable(items[index])) {
            recover()
            return true
        }
        const next = index + (key === Qt.Key_Up || key === Qt.Key_Left ? -1 : 1)
        if (next >= 0 && next < items.length)
            focusControl(items[next])
        return true
    }
    function activate() {
        if (confirmation.item)
            return confirmation.item.activate()
        const item = controls().find(item => item.activeFocus)
        if (!usable(item)) {
            recover()
            return
        }
        item.clicked()
    }
    function back() {
        if (confirmation.active) {
            removingAccount = null
            removingModule = false
            Qt.callLater(restoreFocus)
            return true
        }
        return false
    }
    function openSettings(account) {
        const context = Providers.openSettings(account.id)
        if (context)
            shell.openProviderScreen(context)
        else
            notice = "This account's settings are not available. Open the profile first without changing another viewer's permissions."
    }
    Component.onCompleted: {
        Store.refresh(true)
        Qt.callLater(recover)
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }
    Flickable {
        id: flick
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: content.implicitHeight + Metrics.pageMarginPx * 2
        boundsBehavior: Flickable.StopAtBounds
        ColumnLayout {
            id: content
            width: Math.max(0, Math.min(flick.width - Metrics.pageMarginPx * 2, Metrics.scaled(900)))
            x: (flick.width - width) / 2
            y: Metrics.pageMarginPx
            spacing: Metrics.scaled(18)
            RowLayout {
                Layout.fillWidth: true
                spacing: Metrics.scaled(16)
                IconButton {
                    id: backButton
                    readonly property string focusKey: "back"
                    iconName: "arrow_back"
                    accessibleName: "Back to Sources"
                    onClicked: {
                        root.focusControl(backButton)
                        Router.pop("accounts")
                    }
                }
                ProviderIcon {
                    Layout.preferredWidth: Metrics.scaled(52)
                    Layout.preferredHeight: width
                    source: root.installedModule ? root.installedModule.iconUrl : ""
                    name: root.installedModule ? root.installedModule.name : ""
                    seed: root.moduleId
                }
                AppText {
                    Layout.fillWidth: true
                    text: root.installedModule ? root.installedModule.name : "Provider no longer installed"
                    font.pixelSize: Metrics.titleSizePx
                    font.weight: Font.DemiBold
                    wrapMode: Text.WordWrap
                }
            }
            AppText {
                Layout.fillWidth: true
                visible: Boolean(root.installedModule)
                text: root.installedModule ? "Installed provider version " + root.installedModule.version : ""
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
            }
            SecondaryText {
                Layout.fillWidth: true
                visible: Boolean(root.installedModule)
                text: root.provenanceText + "\nProvider ID: " + root.moduleId
                wrapMode: Text.WordWrap
            }
            SecondaryText {
                Layout.fillWidth: true
                visible: Boolean(root.installedModule && root.installedModule.publisher)
                text: root.installedModule ? "Declared publisher: " + root.installedModule.publisher : ""
                wrapMode: Text.WordWrap
            }
            SecondaryText {
                Layout.fillWidth: true
                visible: Boolean(root.installedModule && root.installedModule.summary)
                text: root.installedModule ? root.installedModule.summary : ""
                wrapMode: Text.WordWrap
            }
            AppText {
                Layout.fillWidth: true
                visible: Boolean(root.installedModule)
                text: root.busy ? "Updating provider…" : root.updateEntry ? "Update available: "
                                                                            + root.updateEntry.version :
                                                                            root.provenance === "native"
                                                                            ? "Updated with Spool" : root.provenance
                                                                              === "file"
                                                                              ? "File updates require a new inspection and explicit trust." :
                                                                                !Store.storeAvailable
                                                                                ? "Provider downloads are disabled in this build." :
                                                                                  Store.loading
                                                                                  ? "Checking provider catalog…" :
                                                                                    Store.error.length > 0
                                                                                    ? Store.error :
                                                                                      "No newer provider version listed."
                wrapMode: Text.WordWrap
            }
            Flow {
                Layout.fillWidth: true
                spacing: Metrics.scaled(12)
                ActionButton {
                    id: updateButton
                    readonly property string focusKey: "update"
                    visible: Boolean(root.installedModule && root.updateEntry && Store.storeAvailable && root.provenance
                                     !== "file")
                    enabled: !root.busy
                    kind: "primary"
                    iconName: "system_update"
                    text: root.updateEntry ? "Update to " + root.updateEntry.version : "Update"
                    onClicked: {
                        root.focusControl(updateButton)
                        Store.update(root.moduleId)
                    }
                }
                ActionButton {
                    id: addButton
                    readonly property string focusKey: "add"
                    visible: Boolean(root.installedModule && root.installedModule.needsAccount)
                    enabled: !root.busy
                    text: "Add an account"
                    iconName: "person_add"
                    onClicked: {
                        root.focusControl(addButton)
                        const context = Providers.beginSetup(root.moduleId)
                        if (context)
                            root.shell.openProviderScreen(context)
                    }
                }
                ActionButton {
                    id: removeButton
                    readonly property string focusKey: "removeProvider"
                    visible: Boolean(root.installedModule && root.installedModule.removable)
                    enabled: !root.busy
                    kind: "flat"
                    text: "Remove provider"
                    onClicked: {
                        root.focusControl(removeButton)
                        root.removingModule = true
                    }
                }
            }
            AppText {
                Layout.fillWidth: true
                text: "Accounts & servers"
                font.pixelSize: Metrics.bodySizePx + Metrics.scaled(4)
                font.weight: Font.DemiBold
            }
            SecondaryText {
                Layout.fillWidth: true
                text: root.accounts.length === 0 ? "No saved accounts. Add an account to configure this provider." :
                                                   "Settings belong to each account. A locked profile must be opened deliberately before its settings are available."
                wrapMode: Text.WordWrap
            }
            Repeater {
                id: accountRepeater
                model: root.accounts
                delegate: ColumnLayout {
                    id: accountRow
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Metrics.scaled(8)
                    function controls() {
                        return [settingsButton, openButton, homeButton, cancelButton, removeAccountButton].filter(item
                                                                                                                  => item.visible
                                                                                                                     && item.enabled)
                    }
                    AppText {
                        Layout.fillWidth: true
                        text: accountRow.modelData.label || accountRow.modelData.providerName
                        font.weight: Font.DemiBold
                        wrapMode: Text.WordWrap
                    }
                    SecondaryText {
                        Layout.fillWidth: true
                        text: [accountRow.modelData.detail, accountRow.modelData.address, accountRow.modelData.pending
                            ? "Opening…" : accountRow.modelData.needsSignIn ? "Sign in again" :
                                                                              accountRow.modelData.running ? (
                                                                                                                 accountRow.modelData.enabled
                                                                                                                 ? "Watching · shown on Home" :
                                                                                                                   "Open · hidden from Home") :
                                                                                                             accountRow.modelData.locked
                                                                                                             ? "PIN required" :
                                                                                                               "Not open"].filter(
                            part => part && part.length > 0).join(" · ")
                        wrapMode: Text.WordWrap
                    }
                    SecondaryText {
                        Layout.fillWidth: true
                        visible: !accountRow.modelData.hasSettings
                        text: "This provider does not offer a separate account settings screen."
                        wrapMode: Text.WordWrap
                    }
                    AppText {
                        Layout.fillWidth: true
                        visible: Boolean(accountRow.modelData.errorText)
                        text: accountRow.modelData.errorText || ""
                        color: Theme.errorText
                        wrapMode: Text.WordWrap
                        Accessible.role: Accessible.AlertMessage
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: Metrics.scaled(12)
                        ActionButton {
                            id: settingsButton
                            readonly property string focusKey: "settings:" + accountRow.modelData.id
                            visible: accountRow.modelData.hasSettings
                            enabled: !accountRow.modelData.pending && !accountRow.modelData.removing && (
                                         accountRow.modelData.running || root.provenance === "native")
                            text: "Account settings"
                            iconName: "settings"
                            onClicked: {
                                root.focusControl(settingsButton)
                                root.openSettings(accountRow.modelData)
                            }
                        }
                        ActionButton {
                            id: openButton
                            readonly property string focusKey: "open:" + accountRow.modelData.id
                            visible: !accountRow.modelData.running || accountRow.modelData.needsSignIn
                            enabled: !accountRow.modelData.pending && !accountRow.modelData.removing
                            text: accountRow.modelData.needsSignIn ? "Sign in again" : "Open profile"
                            onClicked: {
                                root.focusControl(openButton)
                                if (accountRow.modelData.needsSignIn) {
                                    const context = Providers.beginSetup(root.moduleId, accountRow.modelData.id,
                                                                         "reconnect")
                                    if (context)
                                        root.shell.openProviderScreen(context)
                                } else {
                                    Providers.useAccount(accountRow.modelData.id)
                                }
                            }
                        }
                        ActionButton {
                            id: homeButton
                            readonly property string focusKey: "home:" + accountRow.modelData.id
                            visible: accountRow.modelData.running
                            enabled: !accountRow.modelData.pending && !accountRow.modelData.removing
                            text: accountRow.modelData.enabled ? "Hide from Home" : "Show on Home"
                            onClicked: {
                                root.focusControl(homeButton)
                                Providers.setAccountEnabled(accountRow.modelData.id, !accountRow.modelData.enabled)
                            }
                        }
                        ActionButton {
                            id: cancelButton
                            readonly property string focusKey: "cancel:" + accountRow.modelData.id
                            visible: accountRow.modelData.pending
                            text: "Cancel opening"
                            onClicked: {
                                root.focusControl(cancelButton)
                                Providers.cancelActivation(accountRow.modelData.id)
                            }
                        }
                        ActionButton {
                            id: removeAccountButton
                            readonly property string focusKey: "remove:" + accountRow.modelData.id
                            enabled: !accountRow.modelData.removing && !accountRow.modelData.pending
                            text: "Remove account"
                            kind: "flat"
                            onClicked: {
                                root.focusControl(removeAccountButton)
                                root.removingAccount = accountRow.modelData
                            }
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.topMargin: Metrics.scaled(8)
                        implicitHeight: 1
                        color: Theme.border
                    }
                }
            }
            AppText {
                Layout.fillWidth: true
                visible: root.notice.length > 0
                text: root.notice
                wrapMode: Text.WordWrap
                Accessible.role: Accessible.AlertMessage
            }
        }
    }
    Loader {
        id: confirmation
        anchors.fill: parent
        z: 200
        active: root.removingModule || root.removingAccount !== null
        sourceComponent: ConfirmationDialog {
            title: root.removingModule ? "Remove provider?" : "Remove account?"
            message: root.removingModule
                     ? "Remove this provider and its saved accounts from this device? Server accounts and media will not be deleted." :
                       "Remove " + root.removingAccount.label
                       + " from Spool on this device? Their server account and media will not be deleted."
            confirmText: "Remove"
            destructive: true
            onAccepted: {
                if (root.removingModule)
                    Store.uninstall(root.moduleId)
                else
                    Providers.removeAccount(root.removingAccount.id)
                root.removingAccount = null
                root.removingModule = false
                Qt.callLater(root.restoreFocus)
            }
            onDismissed: {
                root.removingAccount = null
                root.removingModule = false
                Qt.callLater(root.restoreFocus)
            }
        }
    }
}
