pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../../theme"
import "../../primitives"

// Choosing a server: type an address, or take one off the network.
//
// The two are one list. What you have typed resolves into the same row shape
// as anything discovered, directly under the field, so there is never a card
// on the page saying one thing and a list underneath saying another.
FocusScope {
    id: root

    // Room is short: the heading and the hint give up their space first.
    property bool dense: false
    readonly property bool scanning: Discovery.active
    readonly property string fallbackServerName: "Jellyfin Server"

    // Live probe of whatever is in the address field. `probeInput` is the text
    // handed to Discovery, so comparing it with the current draft tells us
    // whether the result on screen still describes what the user has typed.
    property string manualDraft: ""
    property string probeInput: ""
    // idle | checking | online | offline
    property string probeState: "idle"
    property string probeAddress: ""
    property string probeServerName: ""
    property string probeStatus: ""
    property string probeDetail: ""
    // Set when the user commits to an address that is still being checked, so
    // the wait costs them nothing: the moment it answers, we move on.
    property bool advanceWhenOnline: false

    readonly property string draftAddress: String(manualDraft || "").trim()
    readonly property bool probeRowVisible: probeInput.length > 0 && probeInput === draftAddress
    readonly property bool probeOnline: probeRowVisible && probeState === "online"

    signal serverChosen(string name, string address)

    // Reaching a server is the slow part of adding an account, so the field
    // starts it as soon as the text names somewhere reachable. Enter still
    // forces an attempt for addresses too unusual to recognise.
    function probeDraft(force) {
        const address = draftAddress
        if (address.length === 0) {
            clearProbe()
            return
        }
        if (!force) {
            if (address === probeInput)
                return
            if (!Discovery.looksLikeServerAddress(address)) {
                clearProbe()
                return
            }
        }
        probeInput = address
        probeAddress = address
        advanceWhenOnline = false
        probeServerName = ""
        probeStatus = ""
        probeDetail = ""
        probeState = "checking"
        Discovery.probeServer(address)
    }

    function clearProbe() {
        Discovery.cancelServerProbe()
        probeInput = ""
        advanceWhenOnline = false
        probeState = "idle"
        probeAddress = ""
        probeServerName = ""
        probeStatus = ""
        probeDetail = ""
    }

    // Enter or a click means "use this one", whatever the probe is doing. If
    // the answer has not landed yet the intent is queued rather than refused.
    function commitProbedServer() {
        if (probeOnline) {
            advanceWhenOnline = false
            App.rememberServer(probeServerName, probeAddress)
            root.serverChosen(probeServerName, probeAddress)
            return
        }
        if (probeState !== "checking")
            probeDraft(true)
        advanceWhenOnline = probeState === "checking"
    }

    // Picking a server that is already listed goes straight to sign-in. A
    // saved entry has been reached before, and re-checking it first would sit
    // the user in front of a spinner to be told what the row already said.
    function chooseDiscoveredServer(index, name, address) {
        if (index < 0)
            return
        App.chooseDiscoveredServer(index)
        root.serverChosen(name, address)
    }

    function controls() {
        const items = [addressRow]
        if (probeRowVisible)
            items.push(probeRow)
        if (discoveredList.count > 0)
            items.push(discoveredList)
        return items
    }

    function focusControl(item) {
        if (item === addressRow)
            addressRow.focusRow()
        else
            InputKeys.focus(item)
    }

    function focusDefault() {
        if (probeRowVisible)
            InputKeys.focus(probeRow)
        else if (discoveredList.count > 0) {
            if (discoveredList.currentIndex < 0)
                discoveredList.currentIndex = 0
            InputKeys.focus(discoveredList)
        } else {
            // The row decides whether that means the caret or the D-pad stop.
            addressRow.focusRow()
        }
    }

    function moveInside(control, key) {
        if (control !== discoveredList || !InputKeys.isVertical(key))
            return false
        const next = discoveredList.currentIndex + (key === Qt.Key_Down ? 1 : -1)
        if (next < 0 || next >= discoveredList.count)
            return false
        discoveredList.currentIndex = next
        return true
    }

    function activateControl(control) {
        if (control === addressRow)
            addressRow.focusField()
        else if (control === probeRow)
            commitProbedServer()
        else if (control === discoveredList && discoveredList.currentItem)
            discoveredList.currentItem.accepted()
    }

    Connections {
        target: Discovery

        function onServerProbeSucceeded(input, server, version, plainHttp) {
            if (input !== root.probeInput)
                return
            root.probeState = "online"
            root.probeAddress = server.address
            root.probeServerName = server.name
            root.probeStatus = plainHttp ? "Online · HTTP" : "Online"
            Session.serverUrl = server.address
            if (root.advanceWhenOnline)
                root.commitProbedServer()
        }

        function onServerProbeFailed(input, message) {
            if (input !== root.probeInput)
                return
            root.probeState = "offline"
            root.advanceWhenOnline = false
            root.probeStatus = TlsTrust.pending ? "Certificate not trusted" : "Couldn't connect"
            root.probeDetail = TlsTrust.pending ? TlsTrust.pendingFingerprint : message
        }
    }

    ColumnLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 0

        // Typing an address and the result of typing it are one thing, so
        // they share one frame. What you get back appears inside the box you
        // typed into rather than as a second card underneath it.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: connectGroup.implicitHeight + Metrics.scaled(40)
            radius: Theme.radiusMedium
            color: "transparent"
            border.width: Math.max(1, Metrics.scaled(1))
            border.color: Theme.border

            ColumnLayout {
                id: connectGroup
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Metrics.scaled(20)
                anchors.rightMargin: Metrics.scaled(20)
                spacing: Metrics.scaled(14)

                AppText {
                    Layout.fillWidth: true
                    visible: !root.dense
                    text: "Sign in to your server"
                    font.pixelSize: Metrics.titleSizePx
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }

                TextFieldRow {
                    id: addressRow
                    Layout.fillWidth: true
                    placeholderText: "Server address"
                    text: root.manualDraft
                    inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                    enterKeyType: Qt.EnterKeyGo
                    onTextEdited: text => {
                        root.manualDraft = text
                        probeDebounce.restart()
                    }
                    // Enter before the debounce has even fired still means
                    // "go": it starts the check and rides it in when it
                    // answers.
                    onAccepted: {
                        probeDebounce.stop()
                        root.commitProbedServer()
                    }
                }

                // Grows out of the field rather than appearing beneath it, so
                // the frame stretches once instead of the page jumping.
                ServerCard {
                    id: probeRow
                    Layout.fillWidth: true
                    Layout.preferredHeight: root.probeRowVisible ? implicitHeight : 0
                    opacity: root.probeRowVisible ? 1 : 0
                    clip: true
                    focus: false
                    // Clickable while it is still checking too: committing
                    // early queues the move rather than turning the click away.
                    selectable: root.probeOnline || root.probeState === "checking"
                    tone: root.probeState === "checking" ? "pending" : root.probeState === "online" ? "positive" :
                                                                                                      "negative"

                    title: root.probeState === "online" && root.probeServerName.length > 0 ? root.probeServerName :
                                                                                             root.probeAddress
                    serverAddress: root.probeState === "online" ? root.probeAddress : ""
                    status: root.probeState === "checking" ? "Connecting…" : root.probeStatus
                    detail: root.probeState === "offline" ? root.probeDetail : ""
                    onAccepted: {
                        InputKeys.focus(probeRow)
                        root.commitProbedServer()
                    }

                    Behavior on Layout.preferredHeight {
                        enabled: !Theme.reducedMotion
                        NumberAnimation {
                            duration: 150
                            easing.type: Easing.OutCubic
                        }
                    }

                    Behavior on opacity {
                        enabled: !Theme.reducedMotion
                        NumberAnimation {
                            duration: 150
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Metrics.scaled(34)
            Layout.bottomMargin: Metrics.scaled(10)
            spacing: Metrics.scaled(10)

            AppText {
                text: "On your network"
                color: Theme.textSecondary
                font.pixelSize: Metrics.metaSizePx
                font.weight: Font.DemiBold
                font.capitalization: Font.AllUppercase
                font.letterSpacing: Math.max(1, Metrics.scaled(1))
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredHeight: Math.max(1, Metrics.scaled(1))
                color: Theme.border
            }

            BusySpinner {
                Layout.alignment: Qt.AlignVCenter
                implicitWidth: Metrics.scaled(13)
                implicitHeight: Metrics.scaled(13)
                visible: root.scanning
                color: Theme.textPrimary
            }

            AppText {
                Layout.alignment: Qt.AlignVCenter
                text: root.scanning ? "Searching…" : discoveredList.count > 0 ? discoveredList.count + " found" : ""
                color: Theme.textPrimary
                font.pixelSize: Metrics.metaSizePx
                font.weight: Font.Medium
            }
        }

        // The well keeps its frame whether or not anything has been found yet,
        // so a server arriving mid-scan slides into a slot that was already
        // there instead of shoving the page around. Two rows is the size of
        // nearly every home network; past that it scrolls rather than growing
        // until the form leaves the screen.
        Rectangle {
            readonly property int rowSpan: Metrics.scaled(64)

            Layout.fillWidth: true
            Layout.minimumHeight: Metrics.scaled(88)
            Layout.preferredHeight: Math.max(Metrics.scaled(88), Math.min(discoveredList.contentHeight + Metrics.scaled(8),
                                                                          Math.round(rowSpan * 2.15)))
            Layout.maximumHeight: Math.round(rowSpan * 2.15)
            radius: Theme.radiusMedium
            color: "transparent"
            border.width: Math.max(1, Metrics.scaled(1))
            border.color: Theme.border

            ListView {
                id: discoveredList
                anchors.fill: parent
                anchors.margins: Metrics.scaled(4)
                visible: count > 0
                clip: true
                spacing: 0
                focus: false
                keyNavigationEnabled: false
                boundsBehavior: Flickable.StopAtBounds
                model: DiscoveredServers
                currentIndex: count > 0 ? 0 : -1
                onCountChanged: {
                    if (count > 0 && !addressRow.editing && !probeRow.activeFocus)
                        Qt.callLater(root.focusDefault)
                }
                onCurrentIndexChanged: if (currentIndex >= 0)
                                           positionViewAtIndex(currentIndex, ListView.Contain)

                FastWheelHandler {
                    flickable: discoveredList
                }

                delegate: ServerCard {
                    id: discoveredRow

                    required property int index
                    required property string name
                    required property string address
                    required property bool online

                    width: discoveredList.width
                    inset: true
                    title: name.length > 0 ? name : root.fallbackServerName
                    serverAddress: address
                    status: online ? "Online" : "Saved"
                    tone: online ? "positive" : "neutral"
                    focused: ListView.isCurrentItem && discoveredList.activeFocus
                    onAccepted: {
                        discoveredList.currentIndex = index
                        InputKeys.focus(discoveredList)
                        root.chooseDiscoveredServer(index, title, address)
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.leftMargin: Metrics.scaled(16)
                        anchors.rightMargin: Metrics.scaled(16)
                        height: Math.max(1, Metrics.scaled(1))
                        visible: discoveredRow.index < discoveredList.count - 1 && !discoveredRow.focused
                        color: Theme.border
                    }
                }
            }

            Column {
                anchors.centerIn: parent
                spacing: Metrics.scaled(7)
                visible: discoveredList.count === 0

                MaterialIcon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    name: root.scanning ? "wifi_tethering" : "wifi_off"
                    iconSize: Metrics.scaled(24)
                    iconColor: Theme.textDisabled
                }

                SecondaryText {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: Math.min(implicitWidth, root.width - Metrics.scaled(48))
                    text: root.scanning ? "Searching your network…" : "No servers found"
                    color: Theme.textMuted
                    font.pixelSize: Metrics.metaSizePx
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }
            }
        }
    }

    Timer {
        id: probeDebounce
        interval: 450
        onTriggered: root.probeDraft(false)
    }
}
