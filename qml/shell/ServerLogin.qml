pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Spool

// Provider operations supply data; layout, focus, cancellation and forms are
// compiled with Spool. Service-specific linking can occupy the alternate slot.
FocusScope {
    id: root
    property var provider
    property string serviceName: ""
    property var errorMessages: ({})
    property string codeLabel: ""
    property string codeStartOperation: ""
    property string codePollOperation: ""
    property string codeInstructions: ""
    property string codeEnabledField: ""
    property string codeAvailableField: ""
    property string alternateLabel: ""
    property Component alternateScreen: null
    property bool alternateActive: false
    property string step: "server"
    property var server: ({})
    property var servers: []
    property bool busy: false
    property bool discovering: false
    property bool validAddress: false
    readonly property bool lanAvailable: !closed && provider.capabilities.lanProbe === true
    property bool lanSearching: false
    property string error: ""
    property string code: ""
    property string secret: ""
    property int generation: 0
    property int validationGeneration: 0
    property int discoveryGeneration: 0
    property int codeGeneration: 0
    readonly property bool closed: !provider || provider.closed
    readonly property real keyboardInset: {
        if (!Qt.inputMethod.visible)
            return 0
        const keyboard = Qt.inputMethod.keyboardRectangle
        if (keyboard.height <= 0)
            return 0
        const pageBottom = root.mapToItem(null, 0, root.height).y
        return Math.max(0, Math.min(root.height, pageBottom - keyboard.y))
    }

    function revealAccountError() {
        if (!accountError.visible || viewport.height <= 0)
            return
        const margin = Metrics.scaled(12)
        const hiddenTop = Math.max(0, -viewport.mapToItem(null, 0, 0).y)
        const errorBounds = accountError.mapToItem(viewport.contentItem, 0, 0, accountError.width, accountError.height)
        let top = errorBounds.y
        let bottom = errorBounds.y + errorBounds.height
        const window = root.Window.window
        const focused = window ? window.activeFocusItem : null
        let ancestor = focused
        while (ancestor && ancestor !== root)
            ancestor = ancestor.parent
        if (focused && focused !== root && ancestor === root) {
            const focusBounds = focused.mapToItem(viewport.contentItem, 0, 0, focused.width, focused.height)
            top = Math.min(top, focusBounds.y)
            bottom = Math.max(bottom, focusBounds.y + focusBounds.height)
        }
        let offset = viewport.contentY
        if (bottom > offset + viewport.height - margin)
            offset = bottom - viewport.height + margin
        if (top < offset + hiddenTop + margin)
            offset = top - hiddenTop - margin
        viewport.contentY = Math.max(0, Math.min(viewport.contentHeight - viewport.height, offset))
    }

    onErrorChanged: {
        if (root.step !== "account" || !root.error.length)
            return
        Qt.callLater(() => {
            if (accountError.visible) {
                accountError.Accessible.announce(root.error, Accessible.Assertive)
                root.revealAccountError()
            }
        })
    }

    Connections {
        target: Qt.inputMethod
        function onVisibleChanged() {
            Qt.callLater(root.revealAccountError)
        }
        function onKeyboardRectangleChanged() {
            Qt.callLater(root.revealAccountError)
        }
        function onAnchorRectangleChanged() {
            Qt.callLater(root.revealAccountError)
        }
    }
    Connections {
        target: root.Window.window
        function onActiveFocusItemChanged() {
            Qt.callLater(root.revealAccountError)
        }
    }

    function validateAddress(input) {
        const stamp = ++validationGeneration
        validAddress = false
        if (!input.trim() || closed)
            return
        provider.request("serverCandidates", {
                             server: input
                         }).then(result => {
                             if (stamp === validationGeneration && !closed)
                                 validAddress = !!result.servers.length
                         }, () => {})
    }
    function mergeServers(found) {
        const merged = servers.slice()
        for (const entry of found) {
            const index = merged.findIndex(row => row.id === entry.id)
            if (index < 0)
                merged.push(entry)
            else
                merged[index] = entry
        }
        servers = merged
    }
    function discoverServers() {
        if (discovering || closed || provider.capabilities.discovery !== true || alternateActive || step !== "server")
            return
        discovering = true
        const stamp = discoveryGeneration
        provider.request("discover").then(result => {
            discovering = false
            if (stamp === discoveryGeneration && !closed)
                mergeServers(result.servers || [])
        }, () => {
            discovering = false
        })
    }
    function cancelLocalSearch() {
        ++discoveryGeneration
        if (lanSearching && !closed)
            provider.cancelLanDiscovery()
        lanSearching = false
    }
    function searchLocalNetwork() {
        if (lanSearching) {
            cancelLocalSearch()
            return
        }
        if (busy || closed)
            return
        const stamp = ++discoveryGeneration
        lanSearching = true
        const cursors = new Set()
        function next(cursor) {
            if (stamp !== discoveryGeneration || closed)
                return
            return provider.request("discoverMore", cursor ? {
                                                                 cursor: cursor
                                                             } : {}).then(result => {
                                                                 if (stamp !== discoveryGeneration || closed)
                                                                     return
                                                                 mergeServers(result.servers || [])
                                                                 if (result.exhausted) {
                                                                     lanSearching = false
                                                                     return
                                                                 }
                                                                 if (!result.cursor || cursors.has(result.cursor)
                                                                         || cursors.size >= 512)
                                                                     throw "invalid_pagination"
                                                                 cursors.add(result.cursor)
                                                                 return next(result.cursor)
                                                             })
        }
        provider.allowLanDiscovery().then(() => next(null)).catch(() => {
            if (stamp === discoveryGeneration) {
                cancelLocalSearch()
                error = "Local search did not finish. Enter a server address or try again."
            }
        })
    }
    function fail(value) {
        busy = false
        error = errorMessages[value] || ({
                                             http_401: "Wrong username or password",
                                             invalid_credentials: "Wrong username or password",
                                             invalid_server: "Enter a valid server address",
                                             origin_denied: "This server address was not allowed"
                                         })[value] || "Couldn't reach the server. Try again."
    }
    function connect(input) {
        if (busy || closed || !String(input).trim())
            return
        cancelLocalSearch()
        const stamp = ++generation
        busy = true
        error = ""
        function current() {
            return stamp === generation && !closed
        }
        function attempt(candidates, index) {
            if (!current())
                return null
            if (!candidates || index >= candidates.length)
                throw "invalid_server"
            const candidate = candidates[index]
            return provider.allowOrigin(candidate).then(() => {
                if (!current())
                    return null
                return provider.request("probe", {
                                            server: candidate
                                        }).catch(reason => {
                                            if (!current())
                                                return null
                                            if (reason !== "cancelled" && reason !== "origin_denied" && index + 1
                                                    < candidates.length)
                                                return attempt(candidates, index + 1)
                                            throw reason
                                        })
            })
        }
        provider.request("serverCandidates", {
                             server: input
                         }).then(result => attempt(result.servers, 0)).then(result => {
                             if (!current() || !result)
                                 return
                             server = result
                             busy = false
                             step = "account"
                             usernameField.text = ""
                             password.text = ""
                             Qt.callLater(() => usernameField.focusRow())
                         }, reason => {
                             if (current())
                                 fail(reason)
                         })
    }
    function signIn(name, value) {
        if (busy || closed)
            return
        const stamp = generation
        busy = true
        error = ""
        password.text = ""
        provider.request("authenticate", {
                             server: server.server,
                             username: name,
                             password: value
                         }).then(account => {
                             if (stamp === generation && !closed)
                                 provider.complete(account)
                         }, reason => {
                             if (stamp === generation && !closed)
                                 fail(reason)
                         })
    }
    function cancelCode() {
        ++codeGeneration
        codePoll.stop()
        code = ""
        secret = ""
    }
    function startCode() {
        if (busy || closed)
            return
        cancelCode()
        const stamp = codeGeneration
        busy = true
        error = ""
        provider.request(codeStartOperation, {
                             server: server.server
                         }).then(result => {
                             if (stamp !== codeGeneration || closed)
                                 return
                             code = result.code
                             secret = result.secret
                             busy = false
                             codePoll.start()
                         }, reason => {
                             if (stamp === codeGeneration && !closed)
                                 fail(reason)
                         })
    }
    function pollCode() {
        const stamp = codeGeneration
        provider.request(codePollOperation, {
                             server: server.server,
                             secret: secret
                         }).then(result => {
                             if (stamp !== codeGeneration || closed)
                                 return
                             if (result.authenticated) {
                                 cancelCode()
                                 provider.complete(result.account)
                             } else
                                 codePoll.start()
                         }, () => {
                             if (stamp === codeGeneration && !closed)
                                 codePoll.start()
                         })
    }
    function retryAvailability() {
        const stamp = generation
        busy = true
        provider.request("probe", {
                             server: server.server
                         }).then(result => {
                             if (stamp === generation && !closed) {
                                 server = result
                                 busy = false
                             }
                         }, reason => {
                             if (stamp === generation && !closed)
                                 fail(reason)
                         })
    }
    function back() {
        if (alternateActive) {
            alternateActive = false
            return true
        }
        if (lanSearching) {
            cancelLocalSearch()
            return true
        }
        if (step === "server" && !busy)
            return false
        ++generation
        cancelCode()
        busy = false
        error = ""
        step = "server"
        password.text = ""
        Qt.callLater(() => address.focusRow())
        return true
    }
    function activate() {
        const item = Window.activeFocusItem
        if (item && typeof item.activate === "function")
            item.activate()
        else if (item && typeof item.clicked === "function")
            item.clicked()
        else if (item && typeof item.accepted === "function")
            item.accepted()
    }
    Component.onCompleted: {
        discoverServers()
        Qt.callLater(() => address.focusRow())
    }
    Component.onDestruction: {
        ++generation
        cancelCode()
        cancelLocalSearch()
    }
    Timer {
        interval: 4000
        repeat: true
        running: !root.closed && root.step === "server" && !root.alternateActive
        onTriggered: root.discoverServers()
    }
    Timer {
        id: validation
        interval: 150
        onTriggered: root.validateAddress(address.text)
    }
    Timer {
        id: codePoll
        interval: 2000
        onTriggered: root.pollCode()
    }

    Flickable {
        id: viewport
        anchors.fill: parent
        anchors.bottomMargin: accountError.visible ? root.keyboardInset : 0
        visible: !root.alternateActive
        contentHeight: Math.max(height, column.implicitHeight + Metrics.pageMarginPx * 2)
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        onHeightChanged: Qt.callLater(root.revealAccountError)
        ColumnLayout {
            id: column
            width: Math.min(parent.width - Metrics.pageMarginPx * 2, Metrics.scaled(680))
            x: (parent.width - width) / 2
            y: Math.max(Metrics.pageMarginPx, (parent.height - implicitHeight) / 2)
            spacing: Metrics.scaled(14)
            // The page header already names the provider; this says what to do.
            AppText {
                Layout.fillWidth: true
                text: root.step === "server" ? "Choose a server" : "Sign in"
                font.pixelSize: Metrics.scaled(40)
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
            }
            AppText {
                Layout.fillWidth: true
                Layout.bottomMargin: Metrics.scaled(10)
                text: root.step === "server" ? "Pick a " + (root.serviceName || "media")
                                               + " server found on your network, or enter its address." :
                                               "Sign in with your " + (root.serviceName || "server") + " account."
                color: Theme.textSecondary
                font.pixelSize: Metrics.scaled(20)
                wrapMode: Text.WordWrap
            }
            Repeater {
                model: root.step === "server" ? root.servers : []
                delegate: ServerCard {
                    required property var modelData
                    Layout.fillWidth: true
                    title: modelData.name
                    serverAddress: modelData.address
                    onAccepted: root.connect(modelData.address)
                }
            }
            SecondaryText {
                Layout.fillWidth: true
                visible: root.step === "server" && !root.servers.length
                text: root.discovering ? "Looking for servers on your network…" :
                                         "No servers found. Enter your server address below."
                wrapMode: Text.WordWrap
            }
            TextFieldRow {
                id: address
                Layout.fillWidth: true
                visible: root.step === "server"
                label: "Server address"
                placeholderText: "192.168.1.20"
                inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                onTextChanged: {
                    root.validAddress = false
                    validation.restart()
                }
                onAccepted: root.connect(text)
            }
            ActionButton {
                Layout.fillWidth: true
                Layout.preferredHeight: Metrics.scaled(56)
                visible: root.step === "server"
                text: root.busy ? "Connecting…" : "Connect"
                iconName: "arrow_forward"
                kind: "primary"
                enabled: root.validAddress && !root.busy
                onClicked: root.connect(address.text)
            }
            SecondaryText {
                Layout.fillWidth: true
                font.pixelSize: Metrics.bodySizePx
                visible: root.step === "server" && address.text.length > 0 && !root.validAddress && !root.busy
                text: "Enter a server address, such as https://media.example.com or 192.168.1.20."
                wrapMode: Text.WordWrap
            }
            SecondaryText {
                Layout.fillWidth: true
                font.pixelSize: Metrics.bodySizePx
                visible: root.step === "server" && root.error.length > 0
                text: root.error
                color: Theme.errorText
                wrapMode: Text.WordWrap
            }
            ActionButton {
                kind: "secondary"
                Layout.fillWidth: true
                visible: root.step === "server" && root.lanAvailable
                iconName: root.lanSearching ? "close" : "search"
                text: root.lanSearching ? "Cancel local search" : "Search local network"
                onClicked: root.searchLocalNetwork()
            }
            ActionButton {
                kind: "secondary"
                Layout.fillWidth: true
                visible: root.step === "server" && !!root.alternateScreen
                text: root.alternateLabel
                enabled: !root.busy
                onClicked: {
                    root.cancelLocalSearch()
                    root.alternateActive = true
                }
            }
            ServerIdentityRow {
                Layout.fillWidth: true
                visible: root.step === "account"
                serverName: root.server.name || root.serviceName
                serverAddress: root.server.server || ""
                onChangeRequested: root.back()
            }
            Flow {
                Layout.fillWidth: true
                visible: root.step === "account" && (root.server.users || []).length > 0
                spacing: Metrics.scaled(12)
                Repeater {
                    model: root.step === "account" ? root.server.users || [] : []
                    delegate: ProfileTile {
                        required property var modelData
                        tileSize: Metrics.scaled(80)
                        username: modelData.name
                        onAccepted: {
                            usernameField.text = modelData.name
                            if (modelData.hasPassword)
                                password.focusRow()
                            else
                                root.signIn(modelData.name, "")
                        }
                    }
                }
            }
            AppText {
                visible: root.step === "account"
                text: "Username"
                font.weight: Font.Medium
            }
            TextFieldRow {
                id: usernameField
                Layout.fillWidth: true
                visible: root.step === "account"
                accessibleName: "Username"
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                enterKeyType: Qt.EnterKeyNext
                onAccepted: password.focusRow()
            }
            AppText {
                visible: root.step === "account"
                text: "Password"
                font.weight: Font.Medium
            }
            TextFieldRow {
                id: password
                Layout.fillWidth: true
                visible: root.step === "account"
                accessibleName: "Password"
                echoMode: TextInput.Password
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                enterKeyType: Qt.EnterKeyGo
                onAccepted: root.signIn(usernameField.text, text)
            }
            RowLayout {
                id: accountError
                Layout.fillWidth: true
                visible: root.step === "account" && root.error.length > 0
                spacing: Metrics.scaled(8)
                Accessible.role: Accessible.StaticText
                Accessible.name: root.error
                Accessible.ignored: !visible
                Accessible.focusable: false
                onHeightChanged: Qt.callLater(root.revealAccountError)
                onYChanged: Qt.callLater(root.revealAccountError)
                MaterialIcon {
                    Layout.alignment: Qt.AlignTop
                    Layout.preferredWidth: iconSize
                    Layout.preferredHeight: iconSize
                    name: "error_outline"
                    iconSize: Metrics.scaled(24)
                    iconColor: Theme.errorText
                    Accessible.ignored: true
                }
                SecondaryText {
                    Layout.fillWidth: true
                    font.pixelSize: Metrics.bodySizePx
                    text: root.error
                    color: Theme.errorText
                    wrapMode: Text.WordWrap
                    Accessible.ignored: true
                }
            }
            ActionButton {
                Layout.fillWidth: true
                Layout.preferredHeight: Metrics.scaled(56)
                visible: root.step === "account"
                text: root.busy ? "Signing in…" : "Sign in"
                iconName: "login"
                kind: "primary"
                enabled: !root.busy && !!usernameField.text.trim()
                onClicked: root.signIn(usernameField.text, password.text)
            }
            ActionButton {
                kind: "secondary"
                Layout.fillWidth: true
                visible: root.step === "account" && !!root.codeLabel && root.server[root.codeAvailableField] === false
                text: "Retry " + root.codeLabel + " availability"
                enabled: !root.busy
                onClicked: root.retryAvailability()
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Metrics.scaled(6)
                visible: root.step === "account" && !!root.codeLabel && root.server[root.codeEnabledField] === true
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: Theme.border
                }
                SecondaryText {
                    text: "or"
                }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: Theme.border
                }
            }
            ActionButton {
                kind: "secondary"
                Layout.fillWidth: true
                visible: root.step === "account" && !!root.codeLabel && root.server[root.codeEnabledField] === true
                text: root.code ? "Cancel " + root.codeLabel : "Use " + root.codeLabel
                iconName: root.code ? "close" : "bolt"
                enabled: !root.busy
                onClicked: root.code ? root.cancelCode() : root.startCode()
            }
            ProviderCodePanel {
                Layout.fillWidth: true
                visible: !!root.code
                code: root.code
                instructions: root.codeInstructions
            }
            BusySpinner {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: Metrics.scaled(24)
                Layout.preferredHeight: Metrics.scaled(24)
                running: root.busy
                visible: running
            }
        }
    }
    Loader {
        anchors.fill: parent
        active: root.alternateActive
        sourceComponent: root.alternateScreen
    }
}
