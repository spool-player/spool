pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

// Who is watching, server by server. Viewers that are alternatives to one
// another -- the users of one server, or one provider's activation family --
// form a set: one person in it watches at a time, and the set decides what
// happens at startup. Independent sets appear on Home together.
FocusScope {
    id: root

    property var shell
    readonly property var routeArgs: shell ? shell.routeArgs : ({})
    // Opened at launch because a set asks who is watching.
    readonly property bool startupMode: Boolean(routeArgs.startup)
    // Opened after signing in, to ask how that set should start.
    readonly property string onboardingId: String(routeArgs.onboarding || "")

    property string pendingAccountId: ""
    property string attemptedPerson: ""
    property string notice: ""
    property var menuPerson: null
    property string startupMenuSet: ""
    property var removingPerson: null
    property bool onboardingAsked: false

    readonly property var sections: {
        const result = []
        for (const account of Providers.accounts) {
            let section = result.find(entry => entry.key === account.profileSet)
            if (!section) {
                section = {
                    key: account.profileSet,
                    moduleId: account.moduleId,
                    providerName: String(account.providerName),
                    iconUrl: account.iconUrl,
                    profiles: Boolean(account.profiles),
                    startupMode: String(account.startupMode || ""),
                    servers: [],
                    people: []
                }
                result.push(section)
            }
            const server = String(account.detail || "")
            if (server.length > 0 && section.servers.indexOf(server) < 0)
                section.servers.push(server)
            let person = section.people.find(entry => entry.key === account.profile)
            if (!person) {
                person = {
                    key: account.profile,
                    set: section.key,
                    moduleId: account.moduleId,
                    label: String(account.label || account.providerName),
                    accounts: []
                }
                section.people.push(person)
            }
            person.accounts.push(account)
        }
        for (const section of result) {
            section.title = section.servers.length === 1 ? section.servers[0] : section.providerName
            section.subtitle = section.servers.length > 1 ? section.servers.join(" · ") : section.providerName
        }
        return result
    }

    function person(key) {
        for (const section of sections) {
            const found = section.people.find(entry => entry.key === key)
            if (found)
                return found
        }
        return null
    }

    function personOf(accountId) {
        for (const section of sections) {
            for (const entry of section.people) {
                if (entry.accounts.some(account => account.id === accountId))
                    return entry
            }
        }
        return null
    }

    function stateOf(entry) {
        const rows = entry ? entry.accounts : []
        if (rows.some(account => account.pending))
            return "pending"
        if (rows.some(account => account.running && account.enabled))
            return "active"
        if (rows.some(account => account.needsSignIn))
            return "signIn"
        if (rows.some(account => account.connectionState === "failed"))
            return "failed"
        if (rows.some(account => account.locked))
            return "locked"
        if (rows.some(account => account.connectionState === "choose"))
            return "choose"
        if (rows.some(account => account.enabled && account.connectionState === "starting"))
            return "starting"
        return "available"
    }

    function stateLabel(state) {
        switch (state) {
        case "pending":
            return "Opening…"
        case "active":
            return "Watching"
        case "signIn":
            return "Sign in again"
        case "failed":
            return "Couldn't open"
        case "locked":
            return "PIN required"
        case "starting":
            return "Connecting…"
        default:
            return ""
        }
    }

    function errorOf(entry) {
        const failing = entry ? entry.accounts.find(account => String(account.errorText || "").length > 0) : null
        return failing ? String(failing.errorText) : ""
    }

    // The single line under the title: what is happening, or what went wrong
    // with the person in question. Tiles carry only a short state.
    readonly property var focusedPerson: {
        const item = activeTile()
        return item ? person(item.personKey) : null
    }
    readonly property string statusText: {
        const pending = pendingAccountId.length > 0 ? personOf(pendingAccountId) : null
        if (pending)
            return "Opening " + pending.label + "… Complete any PIN request, or go back to cancel."
        return statusError.length > 0 ? statusError : notice
    }
    readonly property string statusError: {
        if (pendingAccountId.length > 0)
            return ""
        const failed = focusedPerson && errorOf(focusedPerson).length > 0 ? focusedPerson : person(attemptedPerson)
        return errorOf(failed)
    }

    Connections {
        target: Providers
        function onAccountSelectionFinished(accountId, selected) {
            if (accountId !== root.pendingAccountId)
                return
            root.pendingAccountId = ""
            const entry = root.personOf(accountId)
            if (selected) {
                root.attemptedPerson = ""
                if (root.startupMode && Providers.startupChoicePending) {
                    root.notice = "Now choose who's watching on the next server."
                    return
                }
                root.shell.goHome()
                return
            }
            root.attemptedPerson = entry ? entry.key : ""
            // A removed profile needs no explanation; a cancelled one does.
            if (entry && root.errorOf(entry).length === 0) {
                const section = root.sections.find(candidate => candidate.key === entry.set)
                const active = section ? section.people.find(person => root.stateOf(person) === "active") : null
                root.notice = active ? "Nothing changed. " + active.label + " is still watching." : "Nothing changed."
            }
        }
    }

    function openPerson(entry) {
        if (!entry || pendingAccountId.length > 0)
            return
        notice = ""
        attemptedPerson = entry.key
        const state = stateOf(entry)
        if (state === "pending")
            return
        if (state === "signIn") {
            shell.openProviderScreen(Providers.beginSetup(entry.moduleId))
            return
        }
        if (state === "active") {
            shell.goHome()
            return
        }
        // Any of the person's servers will do: activation brings the rest along.
        const account = entry.accounts.find(row => !row.needsSignIn) || entry.accounts[0]
        pendingAccountId = account.id
        Providers.useAccount(account.id)
    }

    function cancelPending() {
        const entry = pendingAccountId.length > 0 ? personOf(pendingAccountId) : null
        for (const account of entry ? entry.accounts : [])
            Providers.cancelActivation(account.id)
        return Boolean(entry)
    }

    function personActions(entry) {
        const state = stateOf(entry)
        const actions = []
        if (state === "pending")
            actions.push({
                             label: "Cancel opening",
                             value: "cancel"
                         })
        else if (state === "signIn")
            actions.push({
                             label: "Sign in again",
                             value: "signIn"
                         })
        else if (state === "failed")
            actions.push({
                             label: "Try again",
                             value: "open"
                         })
        else if (state !== "active")
            actions.push({
                             label: "Watch as " + entry.label,
                             value: "open"
                         })
        const running = entry.accounts.find(account => account.running && account.enabled)
        if (running && running.hasSettings)
            actions.push({
                             label: "Account settings",
                             value: "settings"
                         })
        if (running)
            actions.push({
                             label: "Hide from Home",
                             value: "hide"
                         })
        actions.push({
                         label: "Remove",
                         value: "remove"
                     })
        return actions
    }

    function runAction(entry, action) {
        if (action === "cancel")
            entry.accounts.forEach(account => Providers.cancelActivation(account.id))
        else if (action === "signIn")
            shell.openProviderScreen(Providers.beginSetup(entry.moduleId))
        else if (action === "open")
            openPerson(entry)
        else if (action === "settings")
            shell.openProviderScreen(Providers.openSettings(entry.accounts.find(account => account.running).id))
        else if (action === "hide")
            entry.accounts.forEach(account => Providers.setAccountEnabled(account.id, false))
        else if (action === "remove")
            removingPerson = entry
    }

    function startupOptions(section) {
        const active = section ? section.people.find(entry => stateOf(entry) === "active") : null
        const options = []
        if (active)
            options.push({
                             label: "Always use " + active.label,
                             mode: "always",
                             account: active.accounts.find(account => account.running).id
                         })
        options.push({
                         label: "Ask who's watching",
                         mode: "ask",
                         account: section ? section.people[0].accounts[0].id : ""
                     })
        return options
    }

    function startupText(section) {
        if (section.startupMode === "ask")
            return "At startup: ask who's watching"
        const pinned = section.people.find(entry => entry.accounts.some(account => account.startupDefault))
        if (section.startupMode === "always" && pinned)
            return "At startup: " + pinned.label
        return "At startup: last profile used"
    }

    // Focus moves between real items: each section offers a row of header
    // controls and a row of people, and the page ends with Add server.
    function focusRows() {
        const rows = []
        for (let i = 0; i < sectionRepeater.count; ++i) {
            const section = sectionRepeater.itemAt(i)
            if (!section)
                continue
            const header = section.headerStops()
            if (header.length)
                rows.push(header)
            const tiles = section.tileStops()
            if (tiles.length)
                rows.push(tiles)
        }
        rows.push([addServerButton])
        return rows
    }

    function focusPosition() {
        const rows = focusRows()
        for (let r = 0; r < rows.length; ++r) {
            const c = rows[r].findIndex(item => item.activeFocus)
            if (c >= 0)
                return {
                    rows: rows,
                    row: r,
                    column: c
                }
        }
        return {
            rows: rows,
            row: -1,
            column: 0
        }
    }

    function activeTile() {
        const position = focusPosition()
        const item = position.row >= 0 ? position.rows[position.row][position.column] : null
        return item && item.personKey !== undefined ? item : null
    }

    function focusStop(item) {
        if (!item)
            return
        InputKeys.focus(item)
        const point = item.mapToItem(flick.contentItem, 0, 0)
        const margin = Metrics.scaled(24)
        if (point.y - margin < flick.contentY)
            flick.contentY = Math.max(0, point.y - margin)
        else if (point.y + item.height + margin > flick.contentY + flick.height)
            flick.contentY = Math.min(Math.max(0, flick.contentHeight - flick.height), point.y + item.height + margin
                                      - flick.height)
    }

    function tileFor(key) {
        return stopFor(key)
    }

    function stopFor(key) {
        for (const row of focusRows()) {
            const found = row.find(item => item.focusKey === key)
            if (found)
                return found
        }
        return null
    }

    // Account updates rebuild the sections; keep the remote on what it was on.
    property string lastFocus: ""
    onSectionsChanged: Qt.callLater(restoreFocus)
    function restoreFocus() {
        if (!root.activeFocus || menuLoader.active || removeConfirmation.active || onboardingDialog.active
                || focusPosition().row >= 0)
            return
        const target = stopFor(lastFocus)
        if (target)
            focusStop(target)
        else
            focusInitial()
    }

    function focusInitial() {
        const rows = focusRows()
        let target = null
        if (onboardingId.length > 0) {
            const entry = personOf(onboardingId)
            target = entry ? tileFor(entry.key) : null
        }
        if (!target && startupMode) {
            const waiting = sections.find(section => section.people.some(entry => stateOf(entry) === "choose"))
            target = waiting ? tileFor(waiting.people[0].key) : null
        }
        if (!target) {
            const tiles = rows.find(row => row.some(item => item.personKey !== undefined))
            target = tiles ? tiles[0] : rows[rows.length - 1][0]
        }
        focusStop(target)
    }

    function routeKey(key, phase, repeat) {
        // Back belongs to back(): the router claims it on press, so a dialog
        // waiting for the release would never hear it.
        if (InputKeys.isBack(key, false, false))
            return false
        if (onboardingDialog.item)
            return onboardingDialog.item.routeKey(key, phase, repeat)
        if (removeConfirmation.item)
            return removeConfirmation.item.routeKey(key, phase, repeat)
        if (menuLoader.item)
            return menuLoader.item.routeKey(key, phase, repeat)
        if (phase !== "press")
            return InputKeys.isDirection(key)
        if (key === Qt.Key_Menu || key === Qt.Key_M)
            return longPress()
        if (!InputKeys.isDirection(key))
            return false
        const position = focusPosition()
        if (position.row < 0) {
            focusInitial()
            return true
        }
        const rows = position.rows
        if (key === Qt.Key_Left || key === Qt.Key_Right) {
            const row = rows[position.row]
            const next = position.column + (key === Qt.Key_Left ? -1 : 1)
            if (next >= 0 && next < row.length)
                focusStop(row[next])
            return true
        }
        const nextRow = position.row + (key === Qt.Key_Up ? -1 : 1)
        if (nextRow < 0) {
            if (shell && !startupMode)
                shell.focusNavBar()
            return true
        }
        if (nextRow < rows.length)
            focusStop(rows[nextRow][Math.min(position.column, rows[nextRow].length - 1)])
        return true
    }

    function activate() {
        if (onboardingDialog.item)
            return onboardingDialog.item.activate()
        if (removeConfirmation.item)
            return removeConfirmation.item.activate()
        if (menuLoader.item)
            return menuLoader.item.activate()
        const position = focusPosition()
        const item = position.row >= 0 ? position.rows[position.row][position.column] : null
        if (!item)
            return focusInitial()
        if (item.personKey !== undefined)
            openPerson(person(item.personKey))
        else
            item.clicked()
    }

    function longPress() {
        const item = activeTile()
        if (!item)
            return false
        openPersonMenu(person(item.personKey), item)
        return true
    }

    function back() {
        if (onboardingDialog.item)
            return onboardingDialog.item.back()
        if (removeConfirmation.item)
            return removeConfirmation.item.back()
        if (menuLoader.item) {
            menuLoader.item.dismissed()
            return true
        }
        if (cancelPending())
            return true
        if (startupMode || onboardingId.length > 0) {
            if (onboardingId.length > 0)
                Providers.finishOnboarding(onboardingId)
            shell.goHome()
            return true
        }
        return false
    }

    function openPersonMenu(entry, anchor) {
        if (!entry)
            return
        startupMenuSet = ""
        menuPerson = entry
        menuAnchor = anchor
        menuLoader.active = true
    }

    function openStartupMenu(section, anchor) {
        menuPerson = null
        startupMenuSet = section.key
        menuAnchor = anchor
        menuLoader.active = true
    }

    property Item menuAnchor: null
    readonly property var startupMenuSection: sections.find(section => section.key === startupMenuSet) || null
    readonly property var onboardingPerson: onboardingId.length > 0 ? personOf(onboardingId) : null
    readonly property bool onboardingReady: Boolean(onboardingPerson && stateOf(onboardingPerson) === "active"
                                                    && sections.length > 0)

    function closeMenu(focusTarget) {
        menuLoader.active = false
        menuPerson = null
        startupMenuSet = ""
        focusStop(focusTarget && focusTarget.visible ? focusTarget : null)
        if (!root.activeTile() && !addServerButton.activeFocus)
            focusInitial()
    }

    onOnboardingReadyChanged: maybeAskOnboarding()
    Component.onCompleted: {
        Qt.callLater(focusInitial)
        Qt.callLater(maybeAskOnboarding)
    }

    function maybeAskOnboarding() {
        if (!onboardingReady || onboardingAsked)
            return
        onboardingAsked = true
        focusStop(tileFor(onboardingPerson.key))
    }

    function answerOnboarding(mode) {
        const account = onboardingPerson ? onboardingPerson.accounts.find(row => row.running) : null
        if (account && mode.length > 0)
            Providers.setStartupChoice(account.id, mode)
        else
            Providers.finishOnboarding(onboardingId)
        shell.goHome()
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: content.implicitHeight + Metrics.pageMarginPx * 2
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        ColumnLayout {
            id: content
            width: Math.min(flick.width - Metrics.pageMarginPx * 2, Metrics.scaled(1080))
            x: (flick.width - width) / 2
            y: Metrics.pageMarginPx
            spacing: Metrics.scaled(20)

            AppText {
                Layout.fillWidth: true
                Layout.topMargin: Metrics.scaled(12)
                text: root.startupMode ? "Who's watching?" : "Profiles & servers"
                font.pixelSize: Metrics.titleSizePx
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
            }

            AppText {
                Layout.fillWidth: true
                text: root.startupMode ? "Choose a profile for each server that asks. Other servers are already open." :
                                         "One person watches on each server at a time. Independent servers appear on Home together."
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
            }

            RowLayout {
                Layout.fillWidth: true
                visible: root.statusText.length > 0
                spacing: Metrics.scaled(12)

                AppText {
                    id: status
                    Layout.fillWidth: true
                    text: root.statusText
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    color: root.statusError.length > 0 ? Theme.errorText : Theme.textSecondary
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
                ActionButton {
                    id: cancelButton
                    visible: root.pendingAccountId.length > 0
                    kind: "flat"
                    text: "Cancel"
                    onClicked: root.cancelPending()
                }
            }

            Repeater {
                id: sectionRepeater
                model: root.sections

                delegate: ColumnLayout {
                    id: sectionItem
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Metrics.scaled(12)

                    function headerStops() {
                        return [startupButton, addProfileButton].filter(item => item.visible)
                    }
                    function tileStops() {
                        const tiles = []
                        for (let i = 0; i < people.count; ++i) {
                            const tile = people.itemAt(i)
                            if (tile)
                                tiles.push(tile)
                        }
                        return tiles
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Metrics.scaled(12)

                        ProviderIcon {
                            Layout.preferredWidth: Metrics.scaled(36)
                            Layout.preferredHeight: Metrics.scaled(36)
                            source: sectionItem.modelData.iconUrl
                            name: sectionItem.modelData.providerName
                            seed: sectionItem.modelData.moduleId
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            AppText {
                                Layout.fillWidth: true
                                text: sectionItem.modelData.title
                                font.pixelSize: Metrics.bodySizePx + 4
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            SecondaryText {
                                Layout.fillWidth: true
                                text: sectionItem.modelData.subtitle
                                elide: Text.ElideRight
                            }
                        }
                        ActionButton {
                            id: startupButton
                            readonly property string focusKey: "startup:" + sectionItem.modelData.key
                            onActiveFocusChanged: if (activeFocus)
                                                      root.lastFocus = focusKey
                            visible: sectionItem.modelData.profiles
                            kind: "flat"
                            iconName: "power_settings_new"
                            text: root.startupText(sectionItem.modelData)
                            onClicked: root.openStartupMenu(sectionItem.modelData, startupButton)
                        }
                        ActionButton {
                            id: addProfileButton
                            readonly property string focusKey: "add:" + sectionItem.modelData.key
                            onActiveFocusChanged: if (activeFocus)
                                                      root.lastFocus = focusKey
                            visible: sectionItem.modelData.profiles
                            kind: "flat"
                            iconName: "person_add"
                            text: "Add profile"
                            onClicked: root.shell.openProviderScreen(Providers.beginSetup(
                                                                         sectionItem.modelData.moduleId))
                        }
                    }

                    Flow {
                        Layout.fillWidth: true
                        spacing: Metrics.scaled(24)

                        Repeater {
                            id: people
                            model: sectionItem.modelData.people

                            delegate: ProfileTile {
                                id: tile
                                required property var modelData
                                readonly property string personKey: modelData.key
                                // Rebuilt tiles must not take focus; restoreFocus decides.
                                focus: false
                                readonly property string focusKey: modelData.key
                                onActiveFocusChanged: if (activeFocus)
                                                          root.lastFocus = focusKey
                                readonly property string profileState: root.stateOf(modelData)
                                tileSize: Metrics.scaled(Metrics.laneAtLeast(root.width, "regular") ? 120 : 96)
                                focused: activeFocus && Metrics.keyboardFocusActive
                                username: modelData.label
                                detail: {
                                    const label = root.stateLabel(tile.profileState)
                                    // One person can span several servers of an activation family.
                                    const servers = modelData.accounts.length > 1 ? modelData.accounts.length
                                                                                    + " servers" : ""
                                    return [label, servers].filter(part => part.length > 0).join(" · ")
                                }
                                detailColor: tile.profileState === "failed" || tile.profileState === "signIn"
                                             ? Theme.errorText : tile.profileState === "active" ? Theme.accent :
                                                                                                  Theme.textMuted
                                badgeIcon: tile.profileState === "locked" ? "lock" : tile.profileState === "signIn"
                                                                            ? "key" : tile.profileState === "failed"
                                                                              ? "error_outline" : ""
                                badgeAlert: tile.profileState === "failed" || tile.profileState === "signIn"
                                busy: tile.profileState === "pending" || tile.profileState === "starting"
                                opacity: tile.profileState === "active" || tile.profileState === "pending" ? 1 : 0.85
                                onAccepted: {
                                    root.focusStop(tile)
                                    root.openPerson(tile.modelData)
                                }
                                onContextRequested: {
                                    root.focusStop(tile)
                                    root.openPersonMenu(tile.modelData, tile)
                                }
                            }
                        }
                    }
                }
            }

            ActionButton {
                id: addServerButton
                readonly property string focusKey: "addServer"
                onActiveFocusChanged: if (activeFocus)
                                          root.lastFocus = focusKey
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: Metrics.scaled(8)
                kind: "secondary"
                iconName: "add"
                text: "Add a server or service"
                onClicked: root.shell.pushRoute("addProvider")
            }
        }
    }

    Loader {
        id: menuLoader
        anchors.fill: parent
        active: false
        z: 200
        sourceComponent: OptionPickerDialog {
            readonly property var actions: root.menuPerson ? root.personActions(root.menuPerson) : root.startupOptions(
                                                                 root.startupMenuSection)
            visible: true
            anchorItem: root.menuAnchor
            title: root.menuPerson ? root.menuPerson.label : "At startup"
            options: actions.map(action => action.label)
            currentIndex: 0
            onSelected: index => {
                const action = actions[index]
                const anchor = root.menuAnchor
                if (root.menuPerson) {
                    const entry = root.menuPerson
                    root.closeMenu(anchor)
                    root.runAction(entry, action.value)
                    return
                }
                root.closeMenu(anchor)
                Providers.setStartupChoice(action.account, action.mode)
            }
            onDismissed: root.closeMenu(root.menuAnchor)
        }
    }

    Loader {
        id: onboardingDialog
        anchors.fill: parent
        active: root.onboardingReady && root.onboardingAsked && root.onboardingPerson.accounts.some(account
                                                                                                    => account.onboarding)

        z: 210
        sourceComponent: ConfirmationDialog {
            readonly property var section: root.sections.find(entry => entry.key === root.onboardingPerson.set)
            title: "When Spool starts"
            message: "Open " + root.onboardingPerson.label + " on " + (section ? section.title : "this server")
                     + " every time, or ask who's watching first? You can change this here later."
            cancelText: "Not now"
            alternativeText: "Ask at startup"
            confirmText: "Always use " + root.onboardingPerson.label
            focusConfirm: true
            onAccepted: root.answerOnboarding("always")
            onAlternativeChosen: root.answerOnboarding("ask")
            onDismissed: root.answerOnboarding("")
        }
    }

    Loader {
        id: removeConfirmation
        anchors.fill: parent
        active: Boolean(root.removingPerson)
        z: 210
        sourceComponent: ConfirmationDialog {
            readonly property bool syncAccount: typeof SettingsSync !== "undefined" && root.removingPerson.accounts.some(
                                                    account => account.id === SettingsSync.accountId)
            title: "Remove profile?"
            message: "Remove " + root.removingPerson.label
                     + " from Spool on this device? Their server account and media will not be deleted." + (syncAccount
                                                                                                            ? " This is your settings sync account. Sync will pause until you choose another account." :
                                                                                                              "")
            confirmText: "Remove"
            destructive: true
            onAccepted: {
                for (const account of root.removingPerson.accounts)
                    Providers.removeAccount(account.id)
                root.removingPerson = null
                Qt.callLater(root.focusInitial)
            }
            onDismissed: {
                const tile = root.tileFor(root.removingPerson.key)
                root.removingPerson = null
                root.focusStop(tile)
            }
        }
    }
}
