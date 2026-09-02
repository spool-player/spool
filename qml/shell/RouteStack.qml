import QtQuick
import JellyfinWebOS
import "../primitives"
import "PageReadiness.js" as PageReadiness

FocusScope {
    id: root

    property string route: "login"
    property var shell
    property bool startupReady: true
    focus: true
    property bool ready: false
    property var uiTransitionToken: 0

    // Resident-page host: pages are created once and route changes switch
    // visibility + focus only. Login is destroyed on leave;
    // itemDetails/personDetails/search are evicted under memory pressure.
    property var pages: ({})
    property var activeLoader: null
    property var pendingLoader: null
    property string activeRoute: ""
    readonly property Item activeItem: activeLoader ? activeLoader.item : null
    readonly property bool directionRelease: Boolean(activeItem && activeItem.directionRelease)

    function pageKey(nextRoute) {
        switch (nextRoute) {
        case "login":
            return "login"
        case "libraryGrid":
            return "libraryGrid"
        case "itemDetails":
            return "itemDetails"
        case "personDetails":
            return "personDetails"
        case "search":
            return "search"
        case "remoteControl":
            return "remoteControl"
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
        case "login":
            return Qt.resolvedUrl("../pages/LoginPage.qml")
        case "libraryGrid":
            return Qt.resolvedUrl("../pages/LibraryGridPage.qml")
        case "itemDetails":
            return Qt.resolvedUrl("../pages/ItemDetailsPage.qml")
        case "personDetails":
            return Qt.resolvedUrl("../pages/PersonDetailsPage.qml")
        case "search":
            return Qt.resolvedUrl("../pages/SearchPage.qml")
        case "remoteControl":
            return Qt.resolvedUrl("../pages/RemoteControlPage.qml")
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

    function loaderFor(key) {
        if (pages[key]) {
            console.info("route host: hit", key)
            return pages[key]
        }
        // A page the type compiler turned into a C++ class is built by asking
        // for the class, not by handing the engine a URL. Everything else
        // still goes through the Loader below, so the two coexist while pages
        // are converted one at a time.
        if (CompiledPageFactory.canCreate(key)) {
            const host = compiledPageComponent.createObject(root)
            host.pageCacheKey = key
            host.item = CompiledPageFactory.create(key, host, root.shell)
            if (host.item) {
                pages[key] = host
                console.info("route host: construct", key, "(compiled)")
                Qt.callLater(root.handleLoaded, host)
                return host
            }
            host.destroy()
        }
        const loader = pageLoaderComponent.createObject(root)
        loader.pageCacheKey = key
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
        const promoted = existing && existing.status === Loader.Loading
        const loader = loaderFor(key)
        pendingLoader = loader
        // Finish in-flight incubation synchronously when someone is actively
        // waiting on this exact page: a promoted prewarm the user beat to the
        // punch, or the startup route while nothing else is on screen yet.
        if (promoted || activeRoute === "")
            loader.asynchronous = false
        const warm = loader.status === Loader.Ready && Boolean(loader.item)
        const cacheHit = warm ? "hit" : promoted ? "promoted" : "miss"
        uiTransitionToken = InputLatency.beginUiTransition("route:" + route + (warm ? ":warm" : ":cold"), activeRoute, route,
                                                           cacheHit)
        settleWatchdog.restart()
        if (warm) {
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
        if (Session.authenticated && !prewarmScheduled) {
            prewarmScheduled = true
            prewarmQueue = ["settings", "subtitleSettings", "libraryGrid", "itemDetails", "personDetails"]
            prewarmTimer.start()
        }
        completeUiTransitionIfReady()
    }

    function dropTransientPages() {
        for (const key of ["login"]) {
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

    function noteUse(key) {
        const order = useOrder.filter(entry => entry !== key)
        order.push(key)
        useOrder = order
    }

    function evictBeyondBudget() {
        const order = useOrder.filter(key => Boolean(pages[key]))
        let excess = order.length - residentPageBudget
        for (let index = 0; index < order.length && excess > 0; ++index) {
            const key = order[index]
            const loader = pages[key]
            if (!loader || loader === activeLoader)
                continue
            delete pages[key]
            loader.destroy()
            --excess
            console.info("route host: released", key)
        }
        useOrder = order.filter(key => Boolean(pages[key]))
    }

    // Memory-pressure eviction: keep the active page plus the cheap,
    // frequently visited residents (home/settings/libraryGrid).
    function trim() {
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
        if (!activeItem || uiTransitionToken === 0)
            return
        if (!PageReadiness.isSettled(activeItem))
            return
        finishUiTransition()
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
                            const key = root.prewarmQueue.shift()
                            if (!root.pages[key]) {
                                root.loaderFor(key)
                                console.info("route host: prewarming", key)
                            }
                            interval = 600
                        }
                    }

                    // Stands in for a Loader around a page that was built as a
                    // C++ class. The route host only ever asks a loader for
                    // pageCacheKey, item, status and visible, so matching that
                    // much keeps one lifecycle for both kinds of page.
                    Component {
                        id: compiledPageComponent

                        Item {
                            id: compiledHost
                            property string pageCacheKey: ""
                            property Item item: null
                            readonly property int status: item ? Loader.Ready : Loader.Null
                            property bool asynchronous: false
                            anchors.fill: parent
                            visible: false

                            // A Loader resizes whatever it loads; nothing does
                            // that for an item handed over from C++. Without
                            // this the page is built at zero size, lays out
                            // nothing, and never reports itself ready -- the
                            // route then waits on a page that is finished but
                            // cannot say so.
                            onItemChanged: {
                                if (!item)
                                    return
                                item.width = Qt.binding(() => compiledHost.width)
                                item.height = Qt.binding(() => compiledHost.height)
                            }
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
