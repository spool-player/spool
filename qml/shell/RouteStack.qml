import QtQuick
import "../primitives"
import "PageReadiness.js" as PageReadiness

FocusScope {
    id: root

    property string route: "home"
    property var shell
    property bool startupReady: true
    focus: true
    property bool ready: false
    property var uiTransitionToken: 0

    // Resident-page host: pages are created once and route changes switch
    // visibility + focus only. Setup pages are destroyed on leave;
    // itemDetails/personDetails/search are evicted under memory pressure.
    property var pages: ({})
    property var activeLoader: null
    property var pendingLoader: null
    property string activeRoute: ""
    // True once the first route has something on screen. The startup overlay
    // waits on this, so what follows the launch screen is a page that has
    // already painted rather than an empty frame of one.
    property bool startupSettled: false
    readonly property Item activeItem: activeLoader ? activeLoader.item : null
    readonly property bool directionRelease: Boolean(activeItem && activeItem.directionRelease)

    function pageKey(nextRoute) {
        switch (nextRoute) {
        case "accounts":
        case "addProvider":
        case "providerScreen":
        case "libraryGrid":
            return nextRoute
        case "itemDetails":
            return "itemDetails"
        case "personDetails":
            return "personDetails"
        case "search":
            return "search"
        case "openSourceNotices":
            return "openSourceNotices"
        case "settings":
            return "settings"
        case "subtitleSettings":
            return "subtitleSettings"
        default:
            return "home"
        }
    }

    function pageSource(key) {
        switch (key) {
        case "accounts":
            return Qt.resolvedUrl("../pages/AccountsPage.qml")
        case "addProvider":
            return Qt.resolvedUrl("../pages/AddProviderPage.qml")
        case "providerScreen":
            return Qt.resolvedUrl("../pages/ProviderScreenPage.qml")
        case "libraryGrid":
            return Qt.resolvedUrl("../pages/LibraryGridPage.qml")
        case "itemDetails":
            return Qt.resolvedUrl("../pages/ItemDetailsPage.qml")
        case "personDetails":
            return Qt.resolvedUrl("../pages/PersonDetailsPage.qml")
        case "search":
            return Qt.resolvedUrl("../pages/SearchPage.qml")
        case "openSourceNotices":
            return Qt.resolvedUrl("../pages/OpenSourceNoticesPage.qml")
        case "settings":
            return Qt.resolvedUrl("../pages/SettingsPage.qml")
        case "subtitleSettings":
            return Qt.resolvedUrl("../pages/SubtitleSettingsPanel.qml")
        default:
            return Qt.resolvedUrl("../pages/HomePage.qml")
        }
    }

    function loaderFor(key, synchronous) {
        if (pages[key]) {
            console.info("route host: hit", key)
            return pages[key]
        }
        const loader = pageLoaderComponent.createObject(root)
        loader.pageCacheKey = key
        // Set before the source, so the very first incubation is the
        // synchronous one. Flipping it afterwards only catches a build that is
        // already under way.
        if (synchronous)
            loader.asynchronous = false
        loader.setSource(pageSource(key), {
                             "shell": root.shell
                         })
        pages[key] = loader
        console.info("route host: construct", key)
        return loader
    }

    function preloadRoute(nextRoute) {
        const key = pageKey(nextRoute)
        if (pages[key])
            return
        loaderFor(key)
        console.info("route host: preloading", key)
    }

    function showRoute() {
        const key = pageKey(route)
        const existing = pages[key]
        // Classify the cache BEFORE loaderFor() can synchronously create a page.
        // Start timing there too: otherwise cold construction disappears from
        // the sample and the newly constructed page is misreported as a hit.
        const promoted = Boolean(existing && existing.status === Loader.Loading)
        const warm = Boolean(existing && existing.status === Loader.Ready && existing.item)
        const cacheHit = warm ? "hit" : promoted ? "promoted" : "miss"
        pendingLoader = null
        uiTransitionToken = InputLatency.beginUiTransition("route:" + route + (warm ? ":warm" : ":cold"), activeRoute, route,
                                                           cacheHit)
        settleWatchdog.restart()

        const loader = loaderFor(key, true)
        pendingLoader = loader
        // Finishing a promoted loader can emit onLoaded synchronously and
        // activate it here. Do not activate the same page a second time below.
        if (promoted || activeRoute === "")
            loader.asynchronous = false
        if (pendingLoader === loader && loader.status === Loader.Ready && Boolean(loader.item)) {
            InputLatency.mark(uiTransitionToken, "instance")
            activatePending()
        }
    }

    function handleLoaded(loader) {
        console.info("route host: ready", loader.pageCacheKey, "objects=" + (loader.item ? loader.item.children.length + 1 :
                                                                                           0))
        if (pendingLoader === loader)
            InputLatency.mark(uiTransitionToken, "instance")
        if (pendingLoader === loader)
            activatePending()
    }

    function activatePending() {
        const loader = pendingLoader
        if (!loader || loader.status !== Loader.Ready || !loader.item)
            return
        pendingLoader = null
        const item = loader.item
        if (typeof item.uiTransitionToken !== "undefined")
            item.uiTransitionToken = uiTransitionToken
        if (item.shell !== root.shell)
            item.shell = root.shell
        if (activeLoader !== loader) {
            const previous = activeLoader
            activeLoader = loader
            loader.visible = true
            if (previous)
                previous.visible = false
        }
        InputKeys.focus(item)
        activeRoute = route
        // A page with nothing to wait for is settled the moment it exists and
        // never emits a readiness change, so ask here as well.
        noteStartupSettled()
        InputLatency.mark(uiTransitionToken, "shell")
        dropTransientPages()
        noteUse(loader.pageCacheKey)
        Qt.callLater(evictBeyondBudget)
        // Every platform amortizes cold construction during idle now. It was
        // desktop-only because an invisible build was thought to block the
        // GUI thread, but the loaders incubate asynchronously: the walk
        // through a cold route shows a worst frame gap under a millisecond
        // while it builds. What made a television slow was never the
        // prewarming; it was dropping every page the moment it left the
        // screen and paying to build it again on the way back.
        if (Providers.hasAccounts && !prewarmScheduled) {
            prewarmScheduled = true
            // Most-wanted first. The budget stops this queue partway through on
            // a small television, so whatever stands at the front is what
            // actually gets built. It used to be the two settings pages, which
            // are rarely opened, while the library and details pages -- the
            // expensive ones on the path everybody walks -- never got a turn.
            prewarmQueue = ["libraryGrid", "itemDetails", "personDetails", "settings", "subtitleSettings"]
            prewarmTimer.start()
        }
        completeUiTransitionIfReady()
    }

    function dropTransientPages() {
        for (const key of ["accounts", "addProvider", "providerScreen"]) {
            const loader = pages[key]
            if (loader && loader !== activeLoader) {
                delete pages[key]
                loader.destroy()
            }
        }
    }

    // How many built pages to keep besides the one on screen. Memory is a
    // real constraint and worth asking about; being a television is not. A
    // set-top box with three gigabytes and a laptop with three gigabytes have
    // the same answer, and the laptop stops being treated as though its
    // memory were free.
    readonly property int residentPageBudget: {
        const bytes = Number(NativeWindow.systemMemoryBytes || 0)
        if (!(bytes > 0))
            return 3
        const gigabytes = bytes / (1024 * 1024 * 1024)
        if (gigabytes < 1.2)
            return 1
        if (gigabytes < 2.5)
            return 3
        return gigabytes < 6 ? 5 : 8
    }

    // Least-recently-shown first, so what gets dropped is what has not been
    // looked at rather than whatever the platform happened to distrust.
    property var useOrder: []

    // Home is the page every journey comes back to and the most expensive one
    // to rebuild, so it is never what gets dropped to make room. Least-recently-
    // shown order was picking it at exactly the wrong moment: while you are
    // several pages deep, home is by definition the thing looked at longest ago,
    // so it went first and the way back cost a full cold build.
    //
    // Empty when the budget cannot afford to hold home as well as whatever is
    // on screen, so the smallest devices keep the budget they were given. Real
    // memory pressure still takes home either way -- see trim().
    readonly property string pinnedPageKey: residentPageBudget >= 2 ? "home" : ""

    function noteUse(key) {
        const order = useOrder.filter(entry => entry !== key)
        order.push(key)
        useOrder = order
    }

    function evictBeyondBudget() {
        const shown = useOrder.filter(key => Boolean(pages[key]))
        // Prewarmed pages never pass through noteUse(), so an order taken from
        // useOrder alone could not see them: the budget said three while the
        // process was holding five. Count every resident page, and spend the
        // excess on the ones nobody has looked at before the ones they have.
        const unshown = Object.keys(pages).filter(key => shown.indexOf(key) < 0)
        const order = unshown.concat(shown)
        let excess = order.length - residentPageBudget
        for (let index = 0; index < order.length && excess > 0; ++index) {
            const key = order[index]
            const loader = pages[key]
            if (!loader || loader === activeLoader || key === pinnedPageKey)
                continue
            delete pages[key]
            loader.destroy();
            --excess
            console.info("route host: released", key)
        }
        useOrder = shown.filter(key => Boolean(pages[key]))
    }

    // Memory-pressure eviction: keep the active page plus the cheap,
    // frequently visited residents (home/settings/libraryGrid).
    function trim() {
        // The platform just asked for memory back, so stop building pages
        // nobody has asked for. The queue is abandoned rather than paused:
        // anything still on it is built on demand if it is ever actually
        // wanted, and rebuilding into sustained pressure is how a low-memory
        // television ends up doing nothing but construct and release.
        prewarmTimer.stop()
        prewarmQueue = []
        const candidates = useOrder.filter(key => Boolean(pages[key]))
        for (const key of candidates) {
            const loader = pages[key]
            if (loader && loader !== activeLoader) {
                delete pages[key]
                loader.destroy()
                console.info("route host: evicted", key, "objects=" + (loader.item ? loader.item.children.length + 1 :
                                                                                     0))
            }
        }
    }

    function finishUiTransition() {
        settleWatchdog.stop()
        InputLatency.mark(uiTransitionToken, "model_ready")
        InputLatency.mark(uiTransitionToken, "content_ready")
        uiTransitionToken = 0
    }

    function completeUiTransitionIfReady() {
        noteStartupSettled()
        if (!activeItem || uiTransitionToken === 0)
            return
        if (!PageReadiness.isSettled(activeItem))
            return
        finishUiTransition()
    }

    function noteStartupSettled() {
        if (!startupSettled && activeItem && PageReadiness.isSettled(activeItem))
            startupSettled = true
    }

    // No page gets to hold a transition open forever. A page whose readiness
    // could never become true -- an empty library waiting on a delegate that
    // was never coming, a settings list that unset its own readiness and had
    // nothing left to set it again -- used to leave the transition unclosed,
    // which held the startup splash up and dropped the route out of the
    // render benchmark without saying so. Closing it late and loudly is
    // better than either.
    //
    // This is a backstop, not a schedule: reaching it means a page is wrong,
    // and the warning is there to be acted on. Long enough that a page which
    // legitimately waits on the network -- person details sits on
    // Content.personItemsBusy -- settles on its own first, and short enough
    // to stay inside the benchmark's own step timeout, so a stuck route
    // records a bad sample rather than vanishing from the report.
    Timer {
        id: settleWatchdog
        interval: 5000
        onTriggered: {
            if (root.uiTransitionToken === 0)
                return
            console.warn("route host:", root.route, "never reported itself settled;", "closing its transition after",
                         interval, "ms")
            root.startupSettled = true
            root.finishUiTransition()
        }
    }

    function beginStartup() {
        if (!startupReady || ready)
            return
        ready = true
        showRoute()
    }

    onRouteChanged: if (ready)
                        showRoute()
    onStartupReadyChanged: beginStartup()
    Component.onCompleted: beginStartup()

    Connections {
        target: root.activeItem
        ignoreUnknownSignals: true
        function onContentReadyChanged() {
            root.completeUiTransitionIfReady()
        }
    }

    function routeKey(key, phase, repeat) {
        return Boolean(activeItem && activeItem.routeKey && activeItem.routeKey(key, phase, repeat))
    }

    // Test and benchmark hook: call a named function on whatever page is on
    // screen, and say whether it happened. This exists so an automated run can
    // drive a page the way a person would -- page down, switch to list -- from
    // outside, without the harness having to know how any page is built. The
    // application itself never calls it.
    function invokeOnActivePage(name: string): var {
    if (!activeItem || typeof activeItem[name] !== "function")
    return undefined
    return activeItem[name]()
}

    function typeAhead(text) {
        return Boolean(activeItem && activeItem.typeAhead && activeItem.typeAhead(text))
    }

        function activate() {
            if (activeItem && activeItem.activate)
            activeItem.activate()
        }

            function longPress() {
                return Boolean(activeItem && activeItem.longPress && activeItem.longPress())
            }

                function back() {
                    return Boolean(activeItem && activeItem.back && activeItem.back())
                }

                    // Cold page construction costs 300-700 ms on TV hardware while warm hits
                    // land in tens of ms, so build every resident page during post-launch
                    // idle, one at a time to keep the GUI thread responsive between them.
                    property var prewarmQueue: []
                    property bool prewarmScheduled: false

                    Timer {
                        id: prewarmTimer
                        interval: 1500
                        repeat: true
                        onTriggered: {
                            if (root.prewarmQueue.length === 0) {
                                stop()
                                return
                            }
                            // Never build past what is going to be kept: filling the budget
                            // with pages nobody asked for would evict the ones they did.
                            if (Object.keys(root.pages).length >= root.residentPageBudget) {
                                stop()
                                return
                            }
                            // Not while somebody is waiting for a page. These are the
                            // expensive pages now that the queue is ordered by cost, and
                            // incubating one while a switch is still coming together spends
                            // the television's one useful core on a page nobody asked for
                            // instead of the one on screen. The queue is not going
                            // anywhere; take the next tick instead.
                            if (root.pendingLoader || (root.activeItem && !PageReadiness.isSettled(root.activeItem)))
                                return
                            const key = root.prewarmQueue.shift()
                            if (!root.pages[key]) {
                                root.loaderFor(key)
                                console.info("route host: prewarming", key)
                            }
                            interval = 600
                        }
                    }

                    Component {
                        id: pageLoaderComponent

                        Loader {
                            id: pageLoader
                            property string pageCacheKey: ""
                            anchors.fill: parent
                            asynchronous: true
                            visible: false
                            onLoaded: root.handleLoaded(pageLoader)
                            onStatusChanged: if (status === Loader.Error)
                                                 console.warn("route host: failed to load", source)
                        }
                    }
                }
