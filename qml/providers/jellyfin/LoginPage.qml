pragma ComponentBehavior: Bound

import QtQuick
import "../../theme"
import "../../primitives"

// The way into the app: pick an account, or add one by finding a server and
// signing in to it.
//
// This file is the frame — which of the three steps is showing, and where a
// key press goes. Each step owns its own state and answers four questions:
// which controls it has, how to focus one, how to move inside one, and what
// Select does to one. That contract is what lets the same single column read
// at arm's length on a laptop, across a room on a television, and pinned above
// an on-screen keyboard on a phone, without any of the steps knowing which it
// is looking at.
FocusScope {
    id: root

    property var shell
    property bool addMode: Session.accountProfiles.length === 0 || Session.profileSignInRequired
    property int addStep: Session.profileSignInRequired ? 2 : 1
    property string selectedServerName: Session.serverName
    property string selectedServerAddress: Session.serverUrl
    property Item backReturnControl: null

    readonly property bool hasSavedAccounts: Session.accountProfiles.length > 0
    // A real on-screen keyboard, not merely a focused field: on desktop the
    // form must not rearrange itself the moment you click into it.
    readonly property bool keyboardVisible: Qt.inputMethod.visible
    // Room, not hardware. A keyboard covering half the panel and a phone held
    // sideways are the same problem and take the same answer: drop everything
    // that is not the form and pin what is left to the top.
    // A step is laid out at its natural size and then shrunk to fit if that
    // does not leave room, rather than being clipped at the bottom edge. The
    // interface scale is a user setting and defaults to 150% on a television,
    // so a form that fits at 100% can easily not fit here -- and the part that
    // falls off is the bottom, which is where Quick Connect's code appears.
    // Scaling keeps every control reachable at any zoom instead of trusting
    // that the content happened to be short enough.
    readonly property real stepFitMargin: Metrics.scaled(12)

    function stepFitScale(needed, available) {
        if (!(needed > 0) || !(available > 0))
            return 1
        const room = available - root.stepFitMargin
        // Below this the text stops being legible across a room, and something
        // has gone wrong that shrinking further will not rescue.
        return Math.max(0.65, Math.min(1, room / needed))
    }

    readonly property bool dense: keyboardVisible || Metrics.units(height) < 620

    readonly property string fallbackServerName: "Jellyfin Server"
    readonly property string chosenServerName: selectedServerName.length > 0 ? selectedServerName : fallbackServerName
    readonly property string chosenServerAddress: selectedServerAddress.length > 0 ? selectedServerAddress :
                                                                                     Session.serverUrl
    readonly property bool canNavigateBack: Boolean(shell && shell.canCancelSwitchUser) || (addMode && (
                                                                                                hasSavedAccounts
                                                                                                || addStep === 2))

    focus: true

    function currentStep() {
        if (!addMode)
            return profilePicker
        return addStep === 2 ? accountStep : serverStep
    }

    function enterProfile(profileId) {
        App.useProfile(profileId)
    }

    function openAddAccount() {
        addMode = true
        addStep = 1
        Qt.callLater(function () {
            serverStep.focusDefault()
        })
    }

    function showProfiles() {
        if (!hasSavedAccounts)
            return
        addMode = false
        Qt.callLater(function () {
            profilePicker.focusDefault()
        })
    }

    function useServer(name, address) {
        selectedServerName = name && name.length > 0 ? name : fallbackServerName
        selectedServerAddress = address
        addStep = 2
        Qt.callLater(function () {
            accountStep.focusDefault()
        })
    }

    function changeServer() {
        addStep = 1
        Qt.callLater(function () {
            serverStep.focusDefault()
        })
    }

    function signIn() {
        App.clearError()
        Session.login()
    }

    function controls() {
        return currentStep().controls()
    }

    function focusedControl() {
        const items = controls()
        for (let i = 0; i < items.length; ++i)
            if (items[i].activeFocus)
                return items[i]
        return items.length > 0 ? items[0] : null
    }

    function focusControl(item) {
        if (item)
            currentStep().focusControl(item)
    }

    function focusBackButton(returnControl) {
        if (!brand.backControl.visible)
            return false
        backReturnControl = returnControl
        InputKeys.focus(brand.backControl)
        return true
    }

    function restoreBackButtonFocus() {
        const items = controls()
        const target = items.indexOf(backReturnControl) >= 0 ? backReturnControl : items.length > 0 ? items[0] : null
        backReturnControl = null
        focusControl(target)
    }

    function moveControl(delta) {
        const items = controls()
        const currentIndex = items.indexOf(focusedControl())
        const nextIndex = Math.max(0, Math.min(items.length - 1, currentIndex + delta))
        if (nextIndex === currentIndex || nextIndex < 0)
            return false
        focusControl(items[nextIndex])
        return true
    }

    function back() {
        if (profileDialogs.open)
            return profileDialogs.back()
        if (addMode && addStep === 2) {
            changeServer()
            return true
        }
        if (addMode && hasSavedAccounts) {
            showProfiles()
            return true
        }
        if (!addMode && shell && shell.canCancelSwitchUser)
            return shell.cancelSwitchUser()
        return false
    }

    function routeKey(key, phase, repeat) {
        if (profileDialogs.open)
            return profileDialogs.routeKey(key, phase, repeat)
        if (InputKeys.isMedia(key) && phase === "press") {
            activate()
            return true
        }
        if (!InputKeys.isDirection(key))
            return false
        if (brand.backControl.activeFocus) {
            if (key === Qt.Key_Right || key === Qt.Key_Down)
                restoreBackButtonFocus()
            return true
        }

        const current = focusedControl()
        // A step gets first refusal, because a grid or a list moves inside
        // itself before the page moves between the things it holds.
        if (currentStep().moveInside(current, key))
            return true
        if (InputKeys.isVertical(key)) {
            const moved = moveControl(key === Qt.Key_Down ? 1 : -1)
            if (!moved && key === Qt.Key_Up)
                focusBackButton(current)
            return true
        }
        // The steps are single columns, so sideways has nowhere to go but out
        // to the one control that is not part of one.
        if (key === Qt.Key_Left)
            focusBackButton(current)
        return true
    }

    function activate() {
        if (profileDialogs.open) {
            profileDialogs.activate()
            return
        }
        if (brand.backControl.activeFocus) {
            brand.backControl.clicked()
            return
        }
        currentStep().activateControl(focusedControl())
    }

    // A remote has no right mouse button, so the account menu hangs off a long
    // press of Select the way it does everywhere else in the app.
    function longPress() {
        if (profileDialogs.open || addMode)
            return false
        return profilePicker.openContextMenu()
    }

    Connections {
        target: Session

        function onProfileSignInRequiredChanged() {
            if (!Session.profileSignInRequired)
                return
            root.selectedServerName = Session.serverName
            root.selectedServerAddress = Session.serverUrl
            root.addMode = true
            root.addStep = 2
            Qt.callLater(function () {
                accountStep.focusDefault()
            })
        }
    }

    Component.onCompleted: Qt.callLater(function () {
        currentStep().focusDefault()
    })

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    LoginBrandPanel {
        id: brand
        x: Metrics.pageMarginPx
        y: root.dense ? Metrics.scaled(8) : Metrics.pageMarginPx
        width: Math.max(0, root.width - Metrics.pageMarginPx * 2)
        height: implicitHeight
        dense: root.dense
        backVisible: root.canNavigateBack
        onBackRequested: root.back()

        Behavior on y {
            enabled: !Theme.reducedMotion
            NumberAnimation {
                duration: 150
                easing.type: Easing.OutCubic
            }
        }
    }

    Item {
        id: stepHost

        // One column at every size. It is capped where a line of text stops
        // being comfortable to scan and centred in whatever is left, so a
        // television and a phone are the same screen at two magnifications
        // rather than two designs.
        readonly property int columnWidth: {
            const preferred = Metrics.scaled(root.addMode ? 620 : 940)
            const available = root.width - Metrics.pageMarginPx * 2
            return Math.max(Metrics.scaled(200), Math.min(preferred, available))
        }

        x: Math.round((root.width - columnWidth) / 2)
        y: brand.y + brand.height + Metrics.scaled(root.dense ? 14 : 34)
        width: columnWidth
        height: Math.max(0, root.height - y - Metrics.pageMarginPx)

        Behavior on y {
            enabled: !Theme.reducedMotion
            NumberAnimation {
                duration: 150
                easing.type: Easing.OutCubic
            }
        }

        LoginProfilePicker {
            id: profilePicker
            anchors.fill: parent
            visible: !root.addMode
            enabled: visible
            dense: root.dense
            onProfileChosen: profileId => root.enterProfile(profileId)
            onAddRequested: root.openAddAccount()
            onContextRequested: (profileId, anchor, serverName, serverUrl) => profileDialogs.show(profileId, anchor, serverName,
                                                                                                  serverUrl)
        }

        // Both forms are short enough to sit off the top edge on a tall screen
        // rather than climbing it, and pinned when there is no room to spare.
        LoginServerStep {
            id: serverStep
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: root.dense ? 0 : Math.max(0, Math.round((parent.height - height) * 0.16))
            readonly property real fitScale: root.stepFitScale(implicitHeight, parent.height)
            height: Math.min(parent.height, implicitHeight * fitScale)
            implicitHeight: childrenRect.height
            scale: fitScale
            transformOrigin: Item.Top
            visible: root.addMode && root.addStep === 1
            enabled: visible
            dense: root.dense
            onServerChosen: (name, address) => root.useServer(name, address)
        }

        LoginAccountStep {
            id: accountStep
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: root.dense ? 0 : Math.max(0, Math.round((parent.height - height) * 0.16))
            readonly property real fitScale: root.stepFitScale(implicitHeight, parent.height)
            height: Math.min(parent.height, implicitHeight * fitScale)
            implicitHeight: childrenRect.height
            scale: fitScale
            transformOrigin: Item.Top
            visible: root.addMode && root.addStep === 2
            enabled: visible
            dense: root.dense
            serverName: root.chosenServerName
            serverAddress: root.chosenServerAddress
            onChangeServerRequested: root.changeServer()
            onSignInRequested: root.signIn()
        }
    }

    LoginProfileDialogs {
        id: profileDialogs
        anchors.fill: parent
        z: 200
        onSignInAgainRequested: {
            root.addMode = true
            root.addStep = 2
        }
        onClosed: Qt.callLater(function () {
            if (!root.addMode)
                profilePicker.focusDefault()
            if (Session.accountProfiles.length === 0)
                root.openAddAccount()
        })
    }
}
