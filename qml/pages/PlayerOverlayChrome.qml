pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import JellyfinWebOS
import "../theme"
import "../primitives"
import "../shell" as Shell

Item {
    id: root

    required property var overlay
    readonly property bool syncPlayMenuOpen: syncPlayMenu.menuOpen
    property int lastPointerX: -1
    property int lastPointerY: -1

    function containsControl(control, position) {
        return control.visible && control.contains(control.mapFromItem(root, position.x, position.y))
    }

    function dp(value) {
        return overlay ? overlay.dp(value) : Math.round(value)
    }
    // Where the pointer was last seen, so movement can be told from mere
    // presence. Seeding it without acting is what makes a pointer that is
    // simply already there -- because the chrome appeared underneath it, or
    // because it was left on the seek bar -- count as nothing.
    function notePointer(position) {
        lastPointerX = Math.round(position.x)
        lastPointerY = Math.round(position.y)
    }

    // A person using the controls moves the pointer. A pointer resting on
    // them does not, and must not renew the hide timer: an overlay raised by
    // somebody scrubbing on another device would otherwise stay up for as
    // long as the pointer happened to be sitting over it. The threshold
    // absorbs the sub-pixel jitter a still hand and a scaled surface produce.
    function pointerMoved(position) {
        const x = Math.round(position.x)
        const y = Math.round(position.y)
        if (lastPointerX < 0 || lastPointerY < 0) {
            notePointer(position)
            return false
        }
        if (Math.abs(x - lastPointerX) < 2 && Math.abs(y - lastPointerY) < 2)
            return false
        lastPointerX = x
        lastPointerY = y
        return true
    }

    function handlePointerMove(position) {
        if (pointerMoved(position))
            overlay.showControlsFromPointer()
    }

    function artworkSource(url) {
        if (!url)
            return ""
        return url.indexOf("http://") === 0 || url.indexOf("https://") === 0 ? "image://artwork/" + encodeURIComponent(
                                                                                   url) : url
    }

    function resetMenu(index) {
        Qt.callLater(function () {
            menuList.currentIndex = menuList.count > 0 ? Math.min(index, menuList.count - 1) : -1
            menuList.clampEnabled()
            if (menuList.currentIndex >= 0)
                menuList.positionViewAtIndex(menuList.currentIndex, ListView.Contain)
        })
    }

    function openSyncPlayMenu() {
        SyncPlay.refreshGroups()
        syncPlayMenu.openMenu()
    }

    function closeSyncPlayMenu() {
        syncPlayMenu.closeMenu()
    }

    function routeSyncPlayMenuKey(key, repeat) {
        return syncPlayMenu.routeKey(key, "press", repeat)
    }

    function activateSyncPlayMenu() {
        syncPlayMenu.activate()
    }

    function routeMenuKey(key, repeat) {
        if (overlay.debugAction(menuList.currentIndex) === "speed" && InputKeys.isHorizontal(key)) {
            overlay.adjustPlaybackSpeed(key === Qt.Key_Left ? -1 : 1)
            return true
        }
        return menuList.routeKey(key, "press", repeat)
    }

    function activateMenu() {
        menuList.activate()
    }

    Item {
        visible: false
        Repeater {
            model: root.overlay.visible && root.overlay.hasPlayer ? root.overlay.player.trickplaySheetUrls : []
            delegate: Image {
                required property string modelData
                source: root.artworkSource(modelData)
                asynchronous: true
                cache: true
            }
        }
    }

    TapHandler {
        property bool surfaceTap: false
        acceptedButtons: Qt.LeftButton
        onPressedChanged: {
            if (!pressed)
            return
            surfaceTap = root.overlay.touchscreenControls && !root.overlay.audioSyncVisible &&
            !root.overlay.subtitleSettingsVisible && !root.overlay.queuePanelVisible &&
            !root.overlay.browsePanelVisible && !root.overlay.isMenuOpen() && !root.syncPlayMenuOpen &&
            !root.containsControl(backButton, point.position) && !root.containsControl(hud, point.position) &&
            !root.containsControl(skipSegment, point.position)
            if (root.overlay.touchscreenControls)
            return
            if (root.syncPlayMenuOpen) {
                const local = syncPlayMenu.mapFromItem(root, point.position.x, point.position.y)
                const syncTarget = actionRow.actionTarget("syncplay")
                if (syncTarget) {
                    const syncLocal = syncTarget.mapFromItem(root, point.position.x, point.position.y)
                    if (syncTarget.contains(syncLocal))
                    return
                }
                if (!syncPlayMenu.contains(local))
                root.overlay.closeMenu()
                return
            }
            root.overlay.showControlsFromPointer()
        }
        onTapped: {
            if (!surfaceTap)
            return
            if (root.overlay.controlsVisible)
            root.overlay.hideControls()
            else
            root.overlay.showControlsFromPointer()
        }
    }
    HoverHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        // Entering is presence, not use: it only records where the pointer
        // is. Point changes are where actual movement shows up.
        onHoveredChanged: if (hovered)
        root.notePointer(point.position)
        onPointChanged: if (hovered)
        root.handlePointerMove(point.position)
    }
    WheelHandler {
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: event => root.overlay.adjustVolumeFromWheel(event)
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: root.dp(150)
        visible: root.overlay.controlsVisible && !root.overlay.audioSyncVisible && !root.overlay.audioOnly
        gradient: Gradient {
            GradientStop {
                position: 0
                color: "#99000000"
            }
            GradientStop {
                position: 1
                color: "transparent"
            }
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: root.dp(360)
        visible: root.overlay.controlsVisible && !root.overlay.audioOnly
        opacity: root.overlay.audioSyncVisible ? 0.35 : 1
        gradient: Gradient {
            GradientStop {
                position: 0
                color: "transparent"
            }
            GradientStop {
                position: 1
                color: Theme.overlayScrimStrong
            }
        }
    }

    Rectangle {
        id: backButton
        readonly property bool focused: root.overlay.isControlsActive() && root.overlay.focusZone === "back"
        readonly property bool highlighted: focused || backHover.hovered
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: root.dp(40)
        width: root.dp(64)
        height: width
        radius: width / 2
        visible: root.overlay.controlsVisible
        color: highlighted ? Qt.alpha(Theme.accent, 0.2) : "transparent"
        border.width: highlighted ? Theme.focusBorderWidth : 0
        border.color: Theme.accent

        MaterialIcon {
            anchors.centerIn: parent
            name: "arrow_back"
            iconColor: backButton.highlighted ? Theme.textPrimary : Theme.textSecondary
            iconSize: root.dp(34)
        }

        HoverHandler {
            id: backHover
        }

        TapHandler {
            onTapped: root.overlay.stopPlayback("overlay-back")
        }
    }

    PlayerTrickplayPreview {
        overlay: root.overlay
    }

    PlayerSkipSegmentCard {
        id: skipSegment
        overlay: root.overlay
    }

    Item {
        id: syncPlayWaitingIcon

        anchors.centerIn: parent
        width: root.dp(116)
        height: width
        visible: SyncPlay.enabled && SyncPlay.waitingForPlayback
        z: 20

        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: "#C9161616"
            border.width: 1
            border.color: Theme.borderStrong
        }

        MaterialIcon {
            anchors.centerIn: parent
            name: "schedule"
            iconColor: Theme.textPrimary
            iconSize: root.dp(82)
        }

        MaterialIcon {
            anchors.centerIn: parent
            anchors.horizontalCenterOffset: root.dp(20)
            anchors.verticalCenterOffset: root.dp(15)
            name: "play_arrow"
            iconColor: Theme.accent
            iconSize: root.dp(38)
        }

        SequentialAnimation on opacity {
            running: syncPlayWaitingIcon.visible && !Theme.reducedMotion
            loops: Animation.Infinite
            NumberAnimation {
                from: 1
                to: 0.58
                duration: 650
            }
            NumberAnimation {
                from: 0.58
                to: 1
                duration: 650
            }
        }
    }

    // The shell's busy overlay stands down while the player owns the screen, so
    // a restart that keeps the player up — a quality change — needs to say so
    // here, in the same badge SyncPlay waits behind.
    Item {
        id: playbackBusyBadge

        anchors.centerIn: parent
        width: root.dp(116)
        height: width
        visible: App.busy && !syncPlayWaitingIcon.visible
        z: 21

        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: "#C9161616"
            border.width: 1
            border.color: Theme.borderStrong
        }

        BusySpinner {
            anchors.centerIn: parent
            width: root.dp(56)
            height: width
            running: playbackBusyBadge.visible
            color: Theme.accent
        }

        AppText {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.bottom
            anchors.topMargin: root.dp(14)
            visible: App.busyText.length > 0
            text: App.busyText
            color: Theme.textSecondary
            font.pixelSize: root.dp(20)
            font.weight: Font.DemiBold
        }
    }

    Item {
        id: hud
        // Video runs the chrome the full width of the picture it belongs to.
        // Audio sets it under the record instead, narrow and centred, which
        // symmetric margins do without fighting the left and right anchors.
        readonly property real sideMargin: root.overlay.audioOnly ? Math.max(root.dp(52), (parent.width - root.dp(860))
                                                                             / 2) : root.dp(52)
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: sideMargin
        // Shorten beside the queue rather than sliding under it. The seek bar
        // and transport row already fill their width, so they simply narrow.
        anchors.rightMargin: sideMargin + (root.overlay.queuePanelVisible ? root.overlay.queuePanelWidth + root.dp(24) :
                                                                            0)
        anchors.bottomMargin: root.dp(52)
        height: root.overlay.audioOnly ? root.dp(176) : root.dp(276)
        // Below this there is no transport bar left worth drawing, only a
        // mangled one.
        visible: root.overlay.controlsVisible && width > root.dp(420)

        ColumnLayout {
            anchors.fill: parent
            // A title on its own sits closer to the clock beneath it; a
            // metadata line in between earns the wider gap.
            spacing: metadataLine.visible ? root.dp(16) : root.dp(8)

            RowLayout {
                Layout.fillWidth: true
                spacing: root.dp(20)

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: metadataLine.visible ? root.dp(6) : 0
                    // Whole pixels: a line's natural height carries a fraction
                    // from the font's metrics, and the line under it inherits
                    // that as its offset. The television rasterizes text with
                    // the platform's own hinting, which has nothing to give a
                    // glyph landing half a pixel down.
                    AppText {
                        Layout.fillWidth: true
                        Layout.preferredHeight: visible ? Math.ceil(implicitHeight) : 0
                        visible: !root.overlay.audioOnly && (!root.overlay.episodeQueue
                                                             || root.overlay.showEpisodeTitle)

                        text: root.overlay.overlayTitle
                        color: Theme.textPrimary
                        font.pixelSize: root.dp(40)
                        font.weight: Font.Bold
                        maximumLineCount: 1
                        elide: Text.ElideRight
                    }

                    AppText {
                        id: metadataLine
                        Layout.fillWidth: true
                        Layout.preferredHeight: visible ? Math.ceil(implicitHeight) : 0
                        visible: !root.overlay.audioOnly && root.overlay.overlayMetadataText.length > 0
                        text: root.overlay.overlayMetadataText
                        color: Theme.textSecondary
                        font.pixelSize: root.dp(22)
                        font.weight: Font.DemiBold
                        maximumLineCount: 1
                        elide: Text.ElideRight
                    }
                }
            }

            PlayerSeekBar {
                overlay: root.overlay
            }

            PlayerTransportBar {
                id: actionRow
                Layout.topMargin: metadataLine.visible ? 0 : root.dp(8)
                overlay: root.overlay
            }

            AppText {
                Layout.fillWidth: true
                visible: root.overlay.hasPlayer && root.overlay.player.errorText.length > 0
                text: root.overlay.hasPlayer ? root.overlay.player.errorText : ""
                color: Theme.errorText
                font.pixelSize: root.dp(18)
                wrapMode: Text.Wrap
            }
        }
    }

    Shell.SyncPlayMenu {
        id: syncPlayMenu
        anchors.right: parent.right
        anchors.bottom: hud.top
        anchors.rightMargin: root.dp(52)
        anchors.bottomMargin: root.dp(18)
        width: root.dp(420)
        z: 55
        onRequestClose: root.overlay.closeMenu()
    }

    PlayerAudioSyncPanel {
        overlay: root.overlay
    }

    // A TV shows this as a modal sheet in the middle of the screen, where the
    // heading is the only thing naming what the remote is pointed at. A
    // desktop opens it from a button that is still on screen, so the same menu
    // belongs under that button as a dropdown and the heading is redundant.
    Item {
        id: menuDialog
        anchors.fill: parent
        visible: root.overlay.menuKind.length > 0
        z: 50

        readonly property bool dropdown: root.overlay.desktopControlsAvailable

        Rectangle {
            anchors.fill: parent
            color: menuDialog.dropdown ? "transparent" : "#99000000"
            MouseArea {
                anchors.fill: parent
                onClicked: root.overlay.closeMenu()
            }
        }

        // Reading menuDialog.visible keeps the placement bound to the moment
        // the menu opens, when the button it hangs from has settled.
        readonly property rect settingsButton: {
            const target = actionRow.actionTarget("debug")
            if (!menuDialog.visible || !target)
            return Qt.rect(0, 0, 0, 0)
            const origin = target.mapToItem(menuDialog, 0, 0)
            return Qt.rect(origin.x, origin.y, target.width, target.height)
        }

        Surface {
            id: menuPanel
            width: menuDialog.dropdown ? root.dp(360) : Math.min(parent.width - root.dp(64), root.dp(760))
            height: Math.min(parent.height - root.dp(64), menuBody.implicitHeight + menuBody.anchors.margins * 2)
            // Centre on the button rather than hanging off the screen edge,
            // clamped so the panel stays inside a narrow window.
            x: {
                if (!menuDialog.dropdown)
                return (parent.width - width) / 2
                const button = menuDialog.settingsButton
                if (button.width <= 0)
                return parent.width - width - root.dp(52)
                const centred = button.x + (button.width - width) / 2
                return Math.max(root.dp(16), Math.min(centred, parent.width - width - root.dp(16)))
            }
            y: {
                if (!menuDialog.dropdown)
                return (parent.height - height) / 2
                const button = menuDialog.settingsButton
                const bottom = button.height > 0 ? button.y : hud.y
                return Math.max(root.dp(16), bottom - height - root.dp(10))
            }
            elevated: true
            clip: true
            baseColor: menuDialog.dropdown ? Theme.bgRaised : Theme.bgPanel

            MouseArea {
                anchors.fill: parent
            }

            ColumnLayout {
                id: menuBody
                anchors.fill: parent
                anchors.margins: menuDialog.dropdown ? root.dp(10) : root.dp(24)
                spacing: menuDialog.dropdown ? root.dp(6) : root.dp(14)

                AppText {
                    Layout.fillWidth: true
                    Layout.preferredHeight: visible ? implicitHeight : 0
                    visible: !menuDialog.dropdown
                    text: root.overlay.menuTitle()
                    color: Theme.textPrimary
                    font.pixelSize: Metrics.titleSizePx
                    font.weight: Font.DemiBold
                }

                AppText {
                    Layout.fillWidth: true
                    Layout.preferredHeight: visible ? implicitHeight : 0
                    visible: menuList.count === 0
                    text: root.overlay.menuPlaceholder()
                    color: Theme.textMuted
                    font.pixelSize: Metrics.bodySizePx
                }

                MenuListView {
                    id: menuList
                    Layout.fillWidth: true
                    // The TV sheet has room for every playback setting; only
                    // unusually long track and queue menus need to scroll.
                    Layout.preferredHeight: !visible ? 0 : menuDialog.dropdown ? contentHeight : Math.min(contentHeight, root.dp(
                                                                                                              480))
                    visible: count > 0
                    model: root.overlay.menuOptions
                    onDismissed: root.overlay.closeMenu()
                    onAccepted: index => root.overlay.activateMenuItem(index)

                    delegate: MenuRow {
                        required property int index
                        required property var modelData
                        label: root.overlay.menuLabel(modelData)
                        detail: root.overlay.menuDetail(index)
                        compact: menuDialog.dropdown
                        checked: root.overlay.menuItemSelected(index)
                        highlighted: menuList.currentIndex === index
                        stepperVisible: root.overlay.debugAction(index) === "speed"
                        stepperEnabled: !SyncPlay.enabled
                        stepperText: root.overlay.formatPlaybackSpeed(root.overlay.player.effectivePlaybackSpeed)
                        stepperEditText: Number(root.overlay.player.effectivePlaybackSpeed || 1).toFixed(2)
                        stepperEditable: stepperVisible && root.overlay.desktopControlsAvailable
                        onStepperAccepted: text => {
                            stepperInvalid = !root.overlay.applyPlaybackSpeedText(text)
                            if (!stepperInvalid)
                                menuList.forceActiveFocus()
                        }
                        onDecreaseRequested: {
                            menuList.currentIndex = index
                            root.overlay.adjustPlaybackSpeed(-1)
                        }
                        onIncreaseRequested: {
                            menuList.currentIndex = index
                            root.overlay.adjustPlaybackSpeed(1)
                        }
                        onHovered: menuList.currentIndex = index
                        onActivated: {
                            menuList.currentIndex = index
                            root.overlay.activateMenuItem(index)
                        }
                    }
                }
            }
        }
    }
}
