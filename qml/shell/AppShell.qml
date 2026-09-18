import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"
import "../providers/jellyfin"
import "RoutePolicy.js" as RoutePolicy

KeyRouter {
    id: root

    readonly property string lane: Metrics.lane(width)
    // A rail across the top of a viewport this narrow is a reach rather than
    // a glance, so it moves to the bottom where a thumb already is.
    readonly property bool navBarAtBottom: lane === "compact"

    // The chrome frames the page that is on screen, not the one just asked
    // for. A route change lands on `route` at once, but the page it names
    // incubates asynchronously -- and the login page is evicted every time it
    // leaves, so it is always a cold load. Sizing the bar from `route` moved
    // and resized the outgoing page in the meantime: leaving home, the whole
    // screen jumped up by the height of the bar and grew into the space it
    // left, held that for a frame or two, and only then was replaced. Coming
    // back it jumped the other way. Following the route that is actually
    // showing leaves the outgoing page alone and moves the chrome in the same
    // frame the incoming page appears.
    readonly property string chromeRoute: routeStack.activeRoute.length > 0 ? routeStack.activeRoute : route

    // The shell is the one place that knows how big the window is and what
    // the user asked the interface to be scaled to. It hands both to Metrics
    // so nothing under qml/theme has to reach for a backend singleton.
    //
    // Bindings rather than onWidthChanged handlers: a handler only runs when
    // the value changes *after* the item exists, so until the platform
    // reported real geometry every size came off the 1920x1080 fallback in
    // Metrics. On a phone or a television reporting half that in logical
    // pixels the settled scale is much smaller, so the first frames drew the
    // top-row icons visibly too large and they shrank on the first update. A
    // binding is evaluated for the first frame as well.
    Binding {
        target: Metrics
        property: "viewportWidth"
        value: root.width
        when: root.width > 0
        restoreMode: Binding.RestoreNone
    }

    Binding {
        target: Metrics
        property: "viewportHeight"
        value: root.height
        when: root.height > 0
        restoreMode: Binding.RestoreNone
    }

    Binding {
        target: Metrics
        property: "zoomPercent"
        value: Settings.uiScalePercent
        restoreMode: Binding.RestoreNone
    }

    Binding {
        target: Metrics
        property: "coarsePointer"
        value: true
        when: Boolean(Platform.touchscreen)
        restoreMode: Binding.RestoreNone
    }

    Binding {
        target: Metrics
        property: "mobileLayout"
        value: Boolean(Platform.touchscreen)
        restoreMode: Binding.RestoreNone
    }

    // Which kind of pointer last touched the app, watched rather than
    // declared: the same build runs on a television with no pointer at all,
    // a desktop with a mouse, and a phone with neither. Both handlers are
    // passive, so nothing here takes an event away from what is under it.
    PointHandler {
        acceptedDevices: PointerDevice.TouchScreen
        onActiveChanged: if (active) {
                             Metrics.coarsePointer = true
                             Metrics.keyboardFocusActive = false
                             Metrics.pointerActive = false
                         }
    }

    // Qt's logical pixel on Android is exactly an Android dp, and Android has
    // already decided what a dp should be for the panel in front of it: a
    // handset reports a few hundred dp across because it is held at arm's
    // length, and a television reports about 960x540 because it is watched
    // from across a room. Scoring that television viewport at 1.0 is what
    // lets both Android form factors start at 100% zoom, instead of a 150%
    // default correcting a desktop-shaped yardstick.
    //
    // sqrt(960 * 540) = 720.
    Binding {
        target: Metrics
        property: "baselinePx"
        value: Platform.isAndroid ? 720 : 1440
        restoreMode: Binding.RestoreNone
    }

    Binding {
        target: Metrics
        property: "pixelsPerMm"
        value: Screen.pixelDensity > 0 ? Screen.pixelDensity : 3.8
        restoreMode: Binding.RestoreNone
    }

    HoverHandler {
        // Android can synthesize mouse hover around a touch sequence. It must
        // not re-enable keyboard focus or resize controls between swipes.
        enabled: !Platform.touchscreen
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onHoveredChanged: if (hovered) {
                              Metrics.coarsePointer = false
                              Metrics.keyboardFocusActive = true
                              Metrics.pointerActive = true
                          }
    }
    focus: true
    backspaceNavigatesInTextInput: Platform.isTV
    webOsScanCodes: Platform.isTV
    platformMayPairHolds: Platform.isAndroid && Platform.isTV

    readonly property string route: Router.route
    readonly property var routeArgs: Router.args || ({})
    // Provider-shaped singletons are only read behind their capability, so a
    // source without sign-in or remote control never has them evaluated. A
    // source with no sign-in is simply always signed in.
    readonly property var session: ProviderCapabilities.auth ? Session : null
    readonly property bool signedIn: session ? session.authenticated : true
    readonly property var remoteControl: ProviderCapabilities.remoteControl ? RemoteControl : null
    readonly property bool remoteTargetSelected: remoteControl ? remoteControl.targetSelected : false
    property bool remoteConnectionKnown: false
    property string lastRemoteTargetName: ""
    onRouteChanged: {
        root.exitArmedAt = 0
        if (route === "remoteControl" && !root.remoteTargetSelected) {
            Qt.callLater(function () {
                if (root.route === "remoteControl" && !root.remoteTargetSelected)
                    root.goHome()
            })
        }
    }

    // Back is how an Android app is left, and at the top of the stack there is
    // nowhere further to go. Nothing here ever exited, so the press did
    // nothing at all and the only way out was the launcher. Two presses leave;
    // the first says so, because back is the most-hit key on a remote and one
    // stray press should not close what someone is halfway through.
    //
    // Nowhere else asks for this. A desktop window is closed, not exited, and
    // webOS has its own way home that does not come through here.
    readonly property bool backExitsAtRoot: Platform.isAndroid
    property int exitConfirmWindowMs: 3000
    property double exitArmedAt: 0
    property bool diagnosticsVisible: false
    property string switchUserReturnProfileId: ""
    property string switchUserReturnRoute: ""
    property var switchUserReturnArgs: ({})
    property bool switchUserReturnPending: false
    readonly property bool canCancelSwitchUser: switchUserReturnProfileId.length > 0
    property bool mediaInfoVisible: false
    property bool itemMenuLoaded: false
    property int uiScaleShortcutKey: 0
    property int searchShortcutKey: 0
    property int settingsShortcutKey: 0
    readonly property bool itemMenuOpen: itemContextMenuLoader.item ? itemContextMenuLoader.item.opened : false
    readonly property bool tlsTrustPending: TlsTrust.pending
    property var mediaInfoItem: ({})
    property bool managementOverlayVisible: false
    property string managementMode: ""
    property var managementItem: ({})
    property var personItem: ({})
    property var pendingPlaybackBackItem: ({})
    textInputActive: Qt.inputMethod.visible || InputKeys.isTextInputItem(root.Window.window
                                                                         ? root.Window.window.activeFocusItem : null)
    property var navigationTarget: routeStack
    activeTarget: updateDialog.open ? updateDialog : tlsTrustPending ? tlsTrustDialog : managementOverlayVisible
                                                                       ? managementOverlayLoader.item : itemMenuOpen
                                                                         ? itemContextMenuLoader.item :
                                                                           mediaInfoVisible
                                                                           ? mediaInfoOverlayLoader.item : hasPlayer
                                                                             && player.visible ? videoSurface :
                                                                                                 navigationTarget
    backHandler: function () {
        return root.back()
    }
    globalHandler: function (key, phase, repeat, modifiers) {
        return root.globalShortcut(key, phase, repeat, modifiers)
    }
    readonly property var player: Player
    readonly property bool hasPlayer: true
    readonly property bool playerSessionActive: hasPlayer && player.sessionActive
    // The player owns the screen for a whole queue step, not only while a file
    // is decoding. mpv drops the surface the instant it reports end-of-file and
    // the next item takes most of a second to negotiate, so keying the shell on
    // player.visible alone flashed the details page between every two tracks.
    readonly property bool playerHoldsScreen: hasPlayer && (player.visible || App.playbackTransition)
    readonly property string errorTextValue: App.errorText
    readonly property bool busyValue: App.busy
    readonly property string busyTextValue: App.busyText
    property real keyboardAvoidance: 0

    // The room the system's own furniture takes: the clock and notifications
    // at the top, the gesture bar at the bottom, a cutout at either side.
    // Read from the shell rather than from the layer it insets, because an
    // item that moves itself by its own safe area never settles.
    readonly property bool interfaceSharesScreen: !(root.hasPlayer && root.playerHoldsScreen)
    readonly property real safeTopPx: interfaceSharesScreen ? root.SafeArea.margins.top : 0
    readonly property real safeBottomPx: interfaceSharesScreen ? root.SafeArea.margins.bottom : 0
    readonly property real safeLeftPx: interfaceSharesScreen ? root.SafeArea.margins.left : 0
    readonly property real safeRightPx: interfaceSharesScreen ? root.SafeArea.margins.right : 0

    // Playback takes the whole panel; everything else leaves the system's
    // bars visible and stays clear of them.
    onInterfaceSharesScreenChanged: NativeWindow.setImmersive(!interfaceSharesScreen)

    function refreshKeyboardAvoidance() {
        if (!Qt.inputMethod.visible) {
            keyboardAvoidance = 0
            return
        }
        const window = root.Window.window
        const focusItem = window ? window.activeFocusItem : null
        if (!InputKeys.isTextInputItem(focusItem) || !focusItem.mapToItem) {
            keyboardAvoidance = 0
            return
        }
        const keyboardRect = Qt.inputMethod.keyboardRectangle
        const keyboardTop = keyboardRect && keyboardRect.height > 0 ? keyboardRect.y : root.height
        const focusPos = focusItem.mapToItem(root, 0, 0)
        const focusBottom = focusPos.y + focusItem.height + keyboardAvoidance
        const overlap = focusBottom + Metrics.scaled(24) - keyboardTop
        keyboardAvoidance = Math.max(0, Math.min(overlap, root.height * 0.45))
    }

    Behavior on keyboardAvoidance {
        enabled: !Theme.reducedMotion
        NumberAnimation {
            duration: 120
            easing.type: Easing.OutCubic
        }
    }

    Connections {
        target: Qt.inputMethod
        function onVisibleChanged() {
            root.refreshKeyboardAvoidance()
        }

        function onKeyboardRectangleChanged() {
            root.refreshKeyboardAvoidance()
        }
        function onAnchorRectangleChanged() {
            root.refreshKeyboardAvoidance()
        }
    }

    Connections {
        target: NativeWindow
        function onPointerBackRequested() {
            root.back()
        }
        function onPointerForwardRequested() {
            root.forward()
        }
        function onPlatformSurfaceExposed(exposed) {
            if (exposed)
                root.restoreInputFocus()
        }
    }

    // Coming back from the background can leave the scene with nothing
    // focused: the window is handed the remote's keys again, but the key
    // router they are meant for no longer has active focus, so every press
    // goes nowhere until something else happens to take it. Put focus back
    // where the shell already says input belongs.
    function restoreInputFocus() {
        const window = root.Window.window
        if (textInputActive || (window && window.activeFocusItem))
            return
        // activeTarget is already the shell's answer to where input belongs,
        // dialog or player or page, so it is the right thing to hand back to.
        InputKeys.focus(activeTarget || navigationTarget)
    }

    Connections {
        target: root.Window.window
        function onActiveFocusItemChanged() {
            root.refreshKeyboardAvoidance()
            // Deferred, because a page being swapped out drops focus for the
            // rest of the turn before whatever replaces it takes focus back.
            // restoreInputFocus stands down if that happened.
            if (!root.Window.window.activeFocusItem)
                Qt.callLater(root.restoreInputFocus)
        }
    }

    Connections {
        target: App
        function onInitializedChanged() {
            if (App.initialized)
                root.applyInitializedRoute()
        }
        function onAggressiveMemoryPressure() {
            if (!root.itemMenuOpen)
                root.itemMenuLoaded = false
            routeStack.trim()
        }
        function onToastMessage(message) {
            toast.show(message)
        }
        function onRemoteUiActionRequested(action) {
            root.handleRemoteUiAction(action)
        }
        function onRemoteMessageRequested(message) {
            remoteMessageText.text = message
            remoteMessage.visible = true
            remoteMessageTimer.restart()
        }
        function onRemoteContentRequested(itemId, itemType, title) {
            root.exitPlaybackForRemoteNavigation("remote-content")
            root.pushRoute("itemDetails", {
                               "itemId": itemId,
                               "itemType": itemType || "Video",
                               "title": title || "Selected item",
                               "returnRoute": root.route
                           })
        }
        function onRemoteSeekPreviewRequested(positionTicks, active) {
            videoSurface.showRemoteSeekPreview(Number(positionTicks) / 10000000, active)
        }
    }

    Connections {
        target: root.remoteControl
        function onTargetChanged() {
            if (root.remoteTargetSelected) {
                const name = root.remoteControl.selectedTargetName || "remote device"
                if (!root.remoteConnectionKnown || root.lastRemoteTargetName !== name)
                    toast.show("Connected to " + name, toast.briefDurationMs)
                root.remoteConnectionKnown = true
                root.lastRemoteTargetName = name
                return
            }
            if (!root.remoteConnectionKnown)
                return
            const targetName = root.lastRemoteTargetName || "remote device"
            root.remoteConnectionKnown = false
            root.lastRemoteTargetName = ""
            if (root.route === "remoteControl")
                root.goHome()
            toast.show("Disconnected from " + targetName, toast.briefDurationMs)
        }
    }
    function showToastAction(message, actionText, callback) {
        toast.showAction(message, actionText, callback)
    }

    function defaultRoute() {
        return root.signedIn ? "home" : "login"
    }

    function restoreRecoveredRoute() {
        if (!Router.recoveryPending || !root.signedIn)
            return false
        const args = root.routeArgs
        if (root.route === "libraryGrid") {
            const libraryId = String(args.libraryId || "")
            if (libraryId.length <= 0) {
                Router.reset("home")
            } else if (!App.openLibraryById(libraryId)) {
                if (Libraries.count > 0)
                    Router.reset("home")
                else
                    return false
            }
        } else if (root.route === "itemDetails") {
            const itemId = String(args.itemId || "")
            if (itemId.length <= 0) {
                Router.reset("home")
            } else {
                Content.prepareLinkedItem(itemId, String(args.title || "Selected item"), String(args.itemType || "Video"),
                                          String(args.seriesId || ""), String(args.title || ""), String(args.seasonId
                                                                                                        || ""))

                const restored = Object.assign({}, args)
                restored.model = Content.linkedItems
                Router.replace("itemDetails", restored)
            }
        } else if (root.route === "personDetails") {
            const personId = String(args.personId || "")
            if (personId.length <= 0) {
                Router.reset("home")
            } else {
                personItem = {
                    id: personId,
                    name: String(args.personName || "Person"),
                    role: String(args.personRole || ""),
                    type: String(args.personType || "Person")
                }
                Content.loadPersonItems(personItem.id)
            }
        }
        Router.finishRecovery()
        return true
    }

    function applyInitializedRoute() {
        if (Router.recoveryPending) {
            if (root.restoreRecoveredRoute())
                return
            if (root.signedIn)
                return
        }
        Router.reset(root.defaultRoute())
    }

    Connections {
        target: root.session
        function onAuthenticatedStateChanged() {
            if (root.signedIn && root.switchUserReturnPending) {
                root.completeSwitchUserReturn()
                return
            }
            if (root.signedIn)
                root.clearSwitchUserReturn()
            if (root.signedIn && root.restoreRecoveredRoute())
                return
            if (root.signedIn)
                Router.replace(root.defaultRoute())
            else
                Router.reset(root.defaultRoute())
        }
    }

    // Every platform states its own text rendering rather than inheriting a
    // default, so that tuning one cannot quietly move another.
    //
    // A television draws a 1080p scene that the panel upscales to 4K over an
    // RGBW subpixel layout: there is no subpixel geometry worth rendering for,
    // and anything soft at 1080p is softer again by the time it reaches the
    // screen. It takes the platform rasterizer with full hinting, which is
    // what keeps stems on whole pixels and glyphs crisp through the upscale.
    //
    // Linux native rendering goes through the platform FreeType/fontconfig
    // path, including the user's antialiasing and subpixel policy. Light
    // hinting keeps baselines aligned without snapping stems to whole pixels,
    // which is what small labels were being coarsened by. Other desktops
    // retain Qt's scalable distance-field rendering.
    Component.onCompleted: {
        // A phone is a finger until something says otherwise. The pointer
        // handlers above only fire once the app has been touched, so without
        // this the first frame is laid out to a mouse's sizes.
        if (!Platform.hasDesktopPointer && !Platform.isTV) {
            Metrics.coarsePointer = true
            Metrics.keyboardFocusActive = false
        }
        if (Platform.isTV) {
            // Full hinting snaps stems to whole pixels, which is what makes
            // the television's text look chiselled at a viewing distance.
            // Hinting the vertical metrics alone keeps the weight and the
            // baseline crisp while curves antialias smoothly.
            Theme.normalTextRenderType = Text.NativeRendering
            Typography.sansHinting = Font.PreferVerticalHinting
        } else if (Qt.platform.os === "linux") {
            Theme.normalTextRenderType = Text.NativeRendering
            Typography.sansHinting = Font.PreferVerticalHinting
        } else {
            Theme.normalTextRenderType = Text.QtRendering
        }
        // The TV keeps its two-step text entry: there the field taking focus
        // is what raises the on-screen keyboard, so the row stays the D-pad
        // target until Select is pressed.
        Theme.textEntryFollowsFocus = !Platform.isTV
        root.remoteConnectionKnown = root.remoteTargetSelected
        root.lastRemoteTargetName = root.remoteTargetSelected ? root.remoteControl.selectedTargetName : ""
        if (App.initialized)
            root.applyInitializedRoute()
    }

    Connections {
        target: Libraries
        function onCountChanged() {
            if (Router.recoveryPending && root.route === "libraryGrid")
                root.restoreRecoveredRoute()
        }
    }

    Connections {
        target: root.player
        function onVisibleChanged() {
            if (root.hasPlayer && root.player.visible) {
                root.preparePlaybackBackNavigation(PlayQueue.currentIndex >= 0 ? PlayQueue.get(PlayQueue.currentIndex) :
                                                                                 ({}))
                root.focusPlayerInput()
            } else if (App.playbackTransition) {
                // Stepping to the next item, not leaving playback. Navigating
                // here would load details — item plus similar items — for the
                // track that just ended, behind a player surface the shell is
                // deliberately still holding. Nobody sees it and it costs a
                // round trip per track.
            } else {
                root.finishPlaybackBackNavigation()
                root.navigationTarget = routeStack
                InputKeys.focus(routeStack)
            }
        }
    }

    function focusPlayerInput() {
        videoSurface.focusInput()
    }

    function preparePlaybackBackNavigation(item) {
        if (RoutePolicy.itemIdFor(item).length <= 0) {
            pendingPlaybackBackItem = ({})
            return
        }
        pendingPlaybackBackItem = item
        routeStack.preloadRoute("itemDetails")
    }

    function finishPlaybackBackNavigation() {
        const item = pendingPlaybackBackItem
        pendingPlaybackBackItem = ({})
        const itemId = RoutePolicy.itemIdFor(item)
        if (itemId.length <= 0)
            return false
        const returnRoute = route === "itemDetails" ? String(routeArgs.returnRoute || "home") : route
        return openDetailsRoute({
                                    "model": PlayQueue,
                                    "itemId": itemId,
                                    "itemType": RoutePolicy.itemTypeFor(item),
                                    "source": "playback",
                                    "returnRoute": returnRoute,
                                    "focusIndex": Math.max(0, PlayQueue.currentIndex)
                                })
    }

    function pushRoute(nextRoute, args) {
        Router.push(nextRoute, args || ({}))
        navigationTarget = routeStack
        InputKeys.focus(routeStack)
    }

    function commitDetailsRoute(args, source, focusIndex) {
        if (!args) {
            console.warn("details route ignored: missing item id", source || "", Math.max(0, Number(focusIndex || 0)))
            return false
        }
        if (RoutePolicy.detailsNavigationMode(route, routeArgs, args, source) === "replace") {
            Router.replace("itemDetails", args)
            InputKeys.focus(routeStack)
        } else {
            pushRoute("itemDetails", args)
        }
        return true
    }

    function openDetailsRoute(request) {
        return commitDetailsRoute(RoutePolicy.normalizeDetailsRoute(request, Browse.items, route), request
                                  ? request.source : "", request ? request.focusIndex : 0)
    }

    function openDetailsAt(model, index, source, returnRoute) {
        const nextModel = model || (Browse.items)
        return commitDetailsRoute(RoutePolicy.detailsRouteAt(nextModel, index, source, returnRoute, route), source,
                                  index)

    }

    function openSeriesDetails(seriesId, seriesName, returnRoute) {
        const id = String(seriesId || "")
        if (id.length <= 0)
            return false
        const title = String(seriesName || "Series")
        Content.prepareLinkedItem(id, title, "Series", "", title, "")
        return openDetailsAt(Content.linkedItems, 0, "series-link", returnRoute || route)
    }

    function openSeasonDetails(seriesId, seasonId, seasonName, seriesName, returnRoute) {
        const showId = String(seriesId || "")
        const id = String(seasonId || "")
        if (showId.length <= 0 || id.length <= 0)
            return false
        Content.prepareLinkedItem(id, String(seasonName || "Season"), "Season", showId, String(seriesName || ""), id)
        return openDetailsAt(Content.linkedItems, 0, "season-link", returnRoute || route)
    }

    function replaceRoute(nextRoute, args) {
        Router.replace(nextRoute, args || ({}))
        navigationTarget = routeStack
        InputKeys.focus(routeStack)
    }

    function goHome() {
        Router.reset("home")
        App.goHome()
        navigationTarget = routeStack
        InputKeys.focus(routeStack)
    }

    function exitPlaybackForRemoteNavigation(reason) {
        if (!playerSessionActive)
            return
        pendingPlaybackBackItem = ({})
        player.stopWithReason(reason)
    }

    function handleRemoteUiAction(action) {
        if (action === "toggle-osd") {
            if (player.visible)
                videoSurface.toggleOsd()
            return
        }
        if (action === "fullscreen") {
            if (!Platform.isTV && !Platform.isAndroid)
                NativeWindow.toggleFullScreen()
            return
        }
        if (action === "context-menu") {
            if (player.visible)
                videoSurface.openPlaybackSettings()
            else
                openContextMenu()
            return
        }
        if (action === "settings") {
            if (player.visible)
                videoSurface.openPlaybackSettings()
            else
                pushRoute("settings")
            return
        }
        if (action === "search") {
            exitPlaybackForRemoteNavigation("remote-search")
            pushRoute("search")
            return
        }
        if (action === "home") {
            exitPlaybackForRemoteNavigation("remote-home")
            goHome()
        }
    }

    function switchUser() {
        if (!root.session)
            return
        switchUserReturnProfileId = root.session.activeProfileId
        switchUserReturnRoute = route
        switchUserReturnArgs = Object.assign({}, routeArgs)
        switchUserReturnPending = false
        Router.reset("login")
        root.session.switchUser()
        navigationTarget = routeStack
        InputKeys.focus(routeStack)
    }

    // The way out of a start that is not arriving: back to the server list,
    // with discovery running again, without waiting for whatever is not
    // answering.
    function chooseServer() {
        switchUser()
        Qt.callLater(function () {
            const page = routeStack.activeItem
            if (page && page.openAddAccount)
                page.openAddAccount()
        })
    }

    function cancelSwitchUser() {
        if (!canCancelSwitchUser || !root.session)
            return false
        if (switchUserReturnPending)
            return true
        switchUserReturnPending = true
        root.session.activateProfile(switchUserReturnProfileId)
        return true
    }

    function completeSwitchUserReturn() {
        if (!switchUserReturnPending || !root.signedIn)
            return false
        const returnRoute = switchUserReturnRoute
        const returnArgs = switchUserReturnArgs
        clearSwitchUserReturn()
        Router.reset(returnRoute, returnArgs)
        navigationTarget = routeStack
        InputKeys.focus(routeStack)
        return true
    }

    function clearSwitchUserReturn() {
        switchUserReturnProfileId = ""
        switchUserReturnRoute = ""
        switchUserReturnArgs = ({})
        switchUserReturnPending = false
    }

    function releaseTextInput() {
        let item = root.Window.window ? root.Window.window.activeFocusItem : null
        while (item) {
            if (item.releaseTextInput && item.releaseTextInput())
                return
            item = item.parent
        }
        Qt.inputMethod.hide()
        InputKeys.focus(routeStack)
    }

    function back() {
        if (updateDialog.open)
            return updateDialog.back()
        if (tlsTrustPending)
            return tlsTrustDialog.back()
        if (textInputActive) {
            releaseTextInput()
            return true
        }
        if (navBar.visible && navBar.remoteControlMenuOpen) {
            navBar.closeRemoteMenu()
            return true
        }
        if (navBar.visible && navBar.syncPlayMenuOpen) {
            navBar.closeSyncPlayMenu()
            return true
        }
        if (diagnosticsVisible) {
            diagnosticsVisible = false
            return true
        }
        if (itemMenuOpen && itemContextMenuLoader.item) {
            itemContextMenuLoader.item.closeMenu()
            return true
        }
        if (managementOverlayVisible) {
            closeManagementOverlay()
            return true
        }
        if (mediaInfoVisible) {
            closeMediaInfo()
            return true
        }
        if (root.hasPlayer && root.player.visible) {
            if (root.player.backAllowed) {
                root.preparePlaybackBackNavigation(PlayQueue.currentIndex >= 0 ? PlayQueue.get(PlayQueue.currentIndex) :
                                                                                 ({}))
                root.player.stopWithReason("shell-back-fallback")
            }
            return true
        }
        if (routeStack.back())
            return true
        if (route === "itemDetails") {
            Router.pop(String(routeArgs.returnRoute || "libraryGrid"))
            InputKeys.focus(routeStack)
            return true
        }
        if (route === "libraryGrid") {
            goHome()
            return true
        }
        if (route === "home" || route === "login")
            return backAtRoot()
        if (Router.canPop) {
            Router.pop(route === "personDetails" ? "itemDetails" : "home")
            InputKeys.focus(routeStack)
            return true
        }
        return false
    }

    function backAtRoot() {
        if (!backExitsAtRoot)
            return false
        if (exitArmedAt > 0 && Date.now() - exitArmedAt <= exitConfirmWindowMs) {
            exitArmedAt = 0
            NativeWindow.exitToLauncher()
            return true
        }
        exitArmedAt = Date.now()
        toast.show("Press back again to exit", toast.briefDurationMs)
        return true
    }

    function forward() {
        if (tlsTrustPending || textInputActive || (navBar.visible && (navBar.syncPlayMenuOpen
                                                                      || navBar.remoteControlMenuOpen))
                || diagnosticsVisible || itemMenuOpen || managementOverlayVisible || mediaInfoVisible
                || playerSessionActive)
            return true
        if (!Router.canForward)
            return false
        if (!Router.forward())
            return false
        navigationTarget = routeStack
        InputKeys.focus(routeStack)
        return true
    }

    function openContextMenu() {
        if (navigationTarget !== routeStack)
            return false
        return routeStack.longPress()
    }

    function openItemMenu(item, anchorItem, context) {
        if (ProviderCapabilities.libraryManagement)
            Management.loadCurrentUserPolicy()
        itemMenuLoaded = true
        return itemContextMenuLoader.item ? itemContextMenuLoader.item.openForItem(item || ({}), anchorItem || null,
                                                                                   context || ({})) : false
    }

    function finishItemMenuOpeningGesture() {
        if (itemContextMenuLoader.item)
            itemContextMenuLoader.item.finishOpeningGesture()
    }

    function restoreFocusAfterItemMenu() {
        if (managementOverlayVisible || mediaInfoVisible || diagnosticsVisible || player.visible)
            return
        navigationTarget = routeStack
        InputKeys.focus(routeStack)
    }

    function mediaInfoAvailable(item) {
        const type = String(item && item.itemType || "")
        return Boolean(item && item.movieId && type !== "Series" && type !== "Season")
    }

    function openMediaInfo(item) {
        if (!mediaInfoAvailable(item))
            return false
        mediaInfoItem = item || ({})
        if (mediaInfoItem.movieId)
            Content.loadItemDetail(mediaInfoItem.movieId)
        mediaInfoVisible = true
        return true
    }

    function closeMediaInfo() {
        mediaInfoVisible = false
        mediaInfoItem = ({})
        InputKeys.focus(routeStack)
    }

    function openManagement(mode, item) {
        if (!ProviderCapabilities.libraryManagement)
            return
        managementMode = mode
        managementItem = item || ({})
        managementOverlayVisible = true
    }

    function closeManagementOverlay() {
        managementOverlayVisible = false
        managementMode = ""
        managementItem = ({})
        Qt.inputMethod.hide()
        InputKeys.focus(routeStack)
    }

    function openPerson(person) {
        personItem = person || ({})
        const personId = String(personItem.id || "")
        if (personId.length <= 0)
            return false
        if (Content)
            Content.loadPersonItems(personId)
        if (route === "personDetails") {
            InputKeys.focus(routeStack)
            return true
        }
        pushRoute("personDetails", {
                      personId: personId,
                      personName: String(personItem.name || "Person"),
                      personRole: String(personItem.role || ""),
                      personType: String(personItem.type || "Person")
                  })
        return true
    }

    function currentMediaItem() {
        const page = routeStack.activeItem
        const item = page && page.currentMediaItem ? page.currentMediaItem() : null
        return item || ({})
    }

    function focusNavBar() {
        if (route === "login")
            return
        const page = routeStack.activeItem
        if (page && page.revealHeader)
            page.revealHeader()
        navigationTarget = navBar
        InputKeys.focus(navBar)
        navBar.focusCurrent()
    }

    function focusContent() {
        if (root.hasPlayer && root.player.visible)
            return
        navigationTarget = routeStack
        InputKeys.focus(routeStack)
    }

    function setUiScale(percent) {
        Settings.setUiScalePercent(Math.max(50, Math.min(180, Math.round(Number(percent || 100) / 5) * 5)))
    }

    function globalShortcut(key, phase, repeat, modifiers) {
        if (phase === "press" && key !== 0)
            Metrics.keyboardFocusActive = true
        if (phase === "release" && key === uiScaleShortcutKey) {
            uiScaleShortcutKey = 0
            return true
        }
        if (phase === "release" && key === searchShortcutKey) {
            searchShortcutKey = 0
            return true
        }
        if (phase === "release" && key === settingsShortcutKey) {
            settingsShortcutKey = 0
            return true
        }
        if (repeat)
            return key === uiScaleShortcutKey || key === searchShortcutKey || key === settingsShortcutKey

        const control = Boolean(modifiers & Qt.ControlModifier)
        const primaryModifier = Boolean(modifiers & (Qt.ControlModifier | Qt.MetaModifier))
        const shortcutRoute = key === Qt.Key_F ? "search" : key === Qt.Key_Comma ? "settings" : ""
        if (phase === "press" && primaryModifier && shortcutRoute.length > 0) {
            if (shortcutRoute === "search")
                searchShortcutKey = key
            else
                settingsShortcutKey = key
            if (playerSessionActive)
                return true
            if (route === shortcutRoute) {
                navigationTarget = routeStack
                InputKeys.focus(routeStack)
            } else {
                pushRoute(shortcutRoute)
            }
            return true
        }
        if (phase === "press" && control) {
            let scaleDelta = 0
            if (key === Qt.Key_Plus || key === Qt.Key_Equal)
                scaleDelta = 5
            else if (key === Qt.Key_Minus || key === Qt.Key_Underscore)
                scaleDelta = -5
            if (scaleDelta !== 0 || key === Qt.Key_0) {
                uiScaleShortcutKey = key
                setUiScale(key === Qt.Key_0 ? 100 : Settings.uiScalePercent + scaleDelta)
                return true
            }
        }

        if (textInputActive)
            return false
        // Claim the physical Menu press as well as its release so focus remains
        // stable until the release-triggered context menu opens.
        if (phase === "press" && (key === Qt.Key_M || key === Qt.Key_Menu))
            return true
        if (phase !== "release")
            return false
        if (control && key === Qt.Key_D) {
            diagnosticsVisible = !diagnosticsVisible
            return true
        }
        if (key === Qt.Key_Slash) {
            pushRoute("search")
            return true
        }
        if (key === Qt.Key_I) {
            if (mediaInfoVisible)
                closeMediaInfo()
            else
                return openMediaInfo(currentMediaItem())
            return true
        }
        if (key === Qt.Key_M || key === Qt.Key_Menu) {
            openContextMenu()
            return true
        }
        if (key === Qt.Key_H || key === Qt.Key_L)
            return deliver(activeTarget, key === Qt.Key_H ? Qt.Key_Left : Qt.Key_Right, "press", false)
        if (key === Qt.Key_Q && root.playerSessionActive) {
            root.player.stopWithReason("shortcut-q")
            return true
        }
        return false
    }

    Item {
        anchors.fill: parent
        z: 1000

        PointHandler {
            acceptedButtons: Qt.LeftButton
            onActiveChanged: if (active) {
                                 if (navBar.remoteControlMenuOpen && !navBar.containsRemoteControlPoint(root,
                                                                                                        point.position.x,
                                                                                                        point.position.y))
                                     navBar.closeRemoteMenu(false)
                                 if (navBar.syncPlayMenuOpen && !navBar.containsSyncPlayPoint(root, point.position.x,
                                                                                              point.position.y))
                                     navBar.closeSyncPlayMenu(false)
                             }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
        visible: !(root.hasPlayer && root.playerHoldsScreen)
    }

    Item {
        id: contentLayer
        objectName: "shellContentLayer"
        anchors.fill: parent
        anchors.topMargin: -root.keyboardAvoidance + root.safeTopPx
        anchors.bottomMargin: root.keyboardAvoidance + root.safeBottomPx
        anchors.leftMargin: root.safeLeftPx
        anchors.rightMargin: root.safeRightPx
        visible: App.initialized && !(root.hasPlayer && root.playerHoldsScreen)
        enabled: visible

        TopBar {
            id: navBar
            objectName: "shellNavigationBar"
            anchors.left: parent.left
            anchors.right: parent.right
            // Keep this as ordinary geometry. Conditional anchor bindings are
            // not ordered when the initial compact lane settles to the real
            // window lane; top and bottom can briefly coexist and leave the
            // bar stretched across the viewport.
            y: root.navBarAtBottom ? Math.max(0, parent.height - height) : 0
            height: root.chromeRoute === "login" ? 0 : Metrics.topBarHeightPx
            edge: root.navBarAtBottom ? "bottom" : "top"
            visible: root.chromeRoute !== "login"
            z: 1
            // Same reason as the height above: the rail marks where you are,
            // not where you are going, so it does not blink its selection off
            // for the frames a page takes to arrive.
            currentRoute: root.chromeRoute
            onActiveFocusChanged: if (activeFocus)
                                      root.navigationTarget = navBar
            onNavigate: r => {
                if (r === "home")
                    root.goHome()
                else if (r === "switchUser")
                    root.switchUser()
                else if (r === "settings")
                    root.pushRoute("settings")
                else
                    root.pushRoute(r)
            }
            onContentRequested: root.focusContent()
        }

        // Space the now-playing bar takes out of the page, so content ends
        // above it rather than under it.
        readonly property real nowPlayingReserve: nowPlayingBar.visible ? nowPlayingBar.height : 0

        RouteStack {
            id: routeStack
            objectName: "shellRouteStack"
            anchors.left: parent.left
            anchors.right: parent.right
            y: root.navBarAtBottom ? 0 : navBar.height
            height: Math.max(0, parent.height - navBar.height - contentLayer.nowPlayingReserve)
            route: root.route
            shell: root
            startupReady: App.initialized
            focus: !(root.hasPlayer && root.playerHoldsScreen)
            onActiveFocusChanged: if (activeFocus)
                                      root.navigationTarget = routeStack
        }

        // Only a source that can drive another device has a bar for it; the
        // loader keeps the bar's bindings from ever running otherwise.
        Loader {
            id: nowPlayingBar
            objectName: "shellRemoteNowPlayingBar"
            anchors.left: parent.left
            anchors.right: parent.right
            // Above the navigation when it sits at the bottom, against the
            // viewport edge when it does not.
            y: root.navBarAtBottom ? Math.max(0, navBar.y - height) : Math.max(0, parent.height - height)
            z: 2
            active: ProviderCapabilities.remoteControl
            height: item ? item.height : 0
            visible: item ? item.visible : false
            sourceComponent: RemoteNowPlayingBar {
                // The remote control page is this bar in full, so it would only
                // duplicate itself there.
                visible: shown && root.chromeRoute !== "remoteControl" && root.chromeRoute !== "login"
                enabled: visible
                onOpenRequested: root.pushRoute("remoteControl")
            }
        }
    }

    // The launch screen, held until the first page has something to show.
    //
    // The bar is the page being settled, not merely created: startup should be
    // the system's launch frame, then this same picture, then a home screen
    // that has already been painted. It is deliberately not artwork -- the
    // first row's delegates existing is enough, and waiting for every visible
    // poster would hold a black screen over a usable page.
    Item {
        id: startupSplash

        readonly property bool contentSettled: routeStack.startupSettled
        property bool dismissed: false
        // A start that is taking this long is a server that is not answering,
        // not a slow machine, so say so and offer the way out rather than
        // leaving a still picture up.
        readonly property bool slow: slowStart.triggered && !dismissed

        anchors.fill: parent
        visible: !dismissed
        enabled: visible
        z: 70

        onContentSettledChanged: if (contentSettled)
                                     dismissed = true

        Timer {
            id: slowStart
            property bool triggered: false
            interval: 1000
            running: !startupSplash.dismissed
            onTriggered: triggered = true
        }

        // The overlay covers the shell, so while it is asking a question the
        // one control on it is where a remote has to land.
        onSlowChanged: if (slow)
                           InputKeys.focus(switchServerButton)
        onDismissedChanged: if (dismissed)
                                InputKeys.focus(routeStack)

        Loader {
            id: splashContent
            anchors.fill: parent
            // The same file the pre-shell frame draws, drawn the same way, so
            // replacing that frame with this one changes nothing on screen.
            // setSource with initial properties, not source + assignment:
            // the values are in place before the component completes, and
            // they come from the singleton rather than the view context,
            // which is what resolves from a component the shell loads.
            Component.onCompleted: setSource("qrc:/startup/SplashContent.qml", {
                                                 "pixelsPerDp": Platform.splashPixelsPerDp,
                                                 "coreWidthDp": Platform.splashCoreWidthDp,
                                                 "coreWidthFraction": Platform.splashCoreWidthFraction,
                                                 "coreAspect": Platform.splashCoreAspect,
                                                 "coreSource": Platform.splashImageUrl
                                             })
        }

        // Grows out of the launch screen rather than replacing it: the mark
        // stays exactly where it was and the waiting appears underneath.
        ColumnLayout {
            anchors.horizontalCenter: parent.horizontalCenter
            y: (splashContent.item ? splashContent.item.markBottomY : parent.height / 2) + Metrics.scaled(36)
            spacing: Metrics.scaled(22)
            opacity: startupSplash.slow ? 1 : 0
            visible: opacity > 0

            Behavior on opacity {
                NumberAnimation {
                    duration: Theme.reducedMotion ? 0 : 180
                }
            }

            BusySpinner {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: Metrics.scaled(34)
                Layout.preferredHeight: Metrics.scaled(34)
                running: startupSplash.slow
                color: Theme.textMuted
            }

            ActionButton {
                id: switchServerButton
                Layout.alignment: Qt.AlignHCenter
                text: "Switch server"
                kind: "secondary"
                onClicked: root.chooseServer()
            }
        }
    }

    VideoSurface {
        id: videoSurface
        anchors.fill: parent
        active: root.hasPlayer && root.player.visible
        mediaInfoVisible: root.mediaInfoVisible
        diagnosticsVisible: root.diagnosticsVisible
        onPlaybackBackRequested: item => root.preparePlaybackBackNavigation(item)
        z: 19
    }

    Loader {
        id: busyOverlayLoader
        anchors.fill: parent
        z: 40
        // Stepping to the next item is the one case where the busy state should
        // show over the player: the surface is being held deliberately and
        // would otherwise be a frozen last frame with no sign of progress.
        active: App.playbackTransition || (root.busyValue && !(root.hasPlayer && root.playerHoldsScreen))
        asynchronous: true
        source: active ? "BusyOverlay.qml" : ""

        Binding {
            target: busyOverlayLoader.item
            property: "text"
            value: App.playbackTransition ? "Loading next item…" : root.busyTextValue
            when: busyOverlayLoader.item
        }
    }

    Loader {
        id: managementOverlayLoader
        anchors.fill: parent
        z: 57
        active: ProviderCapabilities.libraryManagement && root.managementOverlayVisible
        asynchronous: true
        sourceComponent: ManagementDialog {
            mode: root.managementMode
            item: root.managementItem
            onDismissed: root.closeManagementOverlay()
        }
        onLoaded: item.prepare()
    }

    Loader {
        id: itemContextMenuLoader
        anchors.fill: parent
        z: 58
        active: root.itemMenuLoaded
        sourceComponent: ItemContextMenu {
            shell: root
            onClosed: Qt.callLater(root.restoreFocusAfterItemMenu)
        }
    }

    Loader {
        id: mediaInfoOverlayLoader
        anchors.fill: parent
        z: 59
        active: root.mediaInfoVisible
        sourceComponent: MediaInfoOverlay {
            visible: root.mediaInfoVisible
            item: visible ? (root.mediaInfoItem && Object.keys(root.mediaInfoItem).length > 0 ? root.mediaInfoItem :
                                                                                                root.currentMediaItem(
                                                                                                    )) : ({})
            shell: root
            onClosed: root.closeMediaInfo()
        }
    }

    Loader {
        anchors.fill: parent
        z: 61
        active: root.diagnosticsVisible && !(root.hasPlayer && root.playerHoldsScreen)
        sourceComponent: DiagnosticsOverlay {
            route: root.route
            focusedItemId: RoutePolicy.itemIdFor(root.currentMediaItem())
        }
    }
    Rectangle {
        id: remoteMessage
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.round(parent.height * 0.75 - height / 2)
        width: Math.min(parent.width * 0.72, Metrics.scaled(960))
        height: remoteMessageText.implicitHeight + Metrics.scaled(30)
        visible: false
        radius: Theme.radiusMedium
        color: Theme.bgRaised
        z: 69

        AppText {
            id: remoteMessageText
            anchors.centerIn: parent
            width: Math.max(0, parent.width - Metrics.scaled(32))
            color: Theme.textPrimary
            font.pixelSize: Metrics.bodySizePx + Metrics.scaled(1)
            font.weight: Font.Normal
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        Timer {
            id: remoteMessageTimer
            interval: 10000
            onTriggered: remoteMessage.visible = false
        }
    }

    ToastLayer {
        id: toast
        anchors.fill: parent
        // Toasts sit against the bottom edge, which is where the gesture bar
        // is.
        anchors.bottomMargin: root.safeBottomPx
        z: 70
    }

    Surface {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Metrics.scaled(32)
        width: Math.min(parent.width * 0.72, Metrics.scaled(960))
        height: root.errorTextValue.length > 0 ? errorText.implicitHeight + Metrics.scaled(28) : 0
        visible: root.errorTextValue.length > 0
        baseColor: Theme.errorPanel
        z: 80
        AppText {
            id: errorText
            anchors.centerIn: parent
            width: Math.max(0, parent.width - Metrics.scaled(28))
            text: root.errorTextValue
            color: Theme.errorText
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        MouseArea {
            anchors.fill: parent
            onClicked: App.clearError()
        }
    }
    UpdateDialog {
        id: updateDialog
        updater: Platform.updateController
        z: 260
    }
    TlsTrustDialog {
        id: tlsTrustDialog
        visible: root.tlsTrustPending
        trustController: TlsTrust
        inputKeys: InputKeys
        z: 250
    }
    InputLatencyWarning {
        anchors.fill: parent
        z: 90
        monitor: InputLatency
    }
}
