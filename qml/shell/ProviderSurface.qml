import QtQuick
import "../theme"
import "../primitives"

// Hosts one of a provider's own screens: sign-in, account settings or a
// picker. The component gets the context as `provider` and nothing else, and
// the surface closes when the context settles either way.
FocusScope {
    id: root

    property var context: null
    // Over the current page with a scrim, rather than as a page of its own.
    property bool overlay: false
    readonly property var provider: context ? Providers.modules.find(m => m.id === context.moduleId) : null
    readonly property Item screen: loader.item
    property var loadedContext: null
    property bool embedded: false
    signal finished(var result, bool cancelled)

    focus: true

    // The route page is cached and its context arrives after construction,
    // so a screen is (re)loaded whenever the context changes.
    onContextChanged: load()
    function load() {
        if (context === loadedContext)
            return
        loadedContext = context || null
        if (loadedContext)
            loader.setSource(loadedContext.component, {
                                 "provider": loadedContext
                             })
        else
            loader.source = ""
    }

    // Provider screens are ordinary QML forms. Where they route keys
    // themselves they get first say; otherwise Up and Down walk the focus
    // chain, which is enough for a D-pad to reach every field and button.
    function routeKey(key, phase, repeat) {
        if (screen && screen.routeKey && screen.routeKey(key, phase, repeat))
            return true
        if (phase !== "press")
            return false
        const current = Window.activeFocusItem
        if (!current || InputKeys.isTextInputItem(current))
            return false
        if (key === Qt.Key_Down || key === Qt.Key_Up) {
            const next = current.nextItemInFocusChain(key === Qt.Key_Down)
            if (next && root.contains(root.mapFromItem(next, 0, 0)))
                InputKeys.focus(next)
            return true
        }
        return false
    }

    function activate() {
        if (screen && screen.activate)
            return screen.activate()
        const item = Window.activeFocusItem
        if (item && typeof item.activate === "function")
            item.activate()
        else if (item && typeof item.clicked === "function")
            item.clicked()
    }

    function back() {
        if (screen && screen.back && screen.back())
            return true
        if (context)
            context.close()
        return true
    }

    Connections {
        target: root.context || null
        function onFinished(result, cancelled) {
            root.finished(result, cancelled)
        }
    }

    Rectangle {
        anchors.fill: parent
        color: root.embedded ? "transparent" : root.overlay ? Theme.overlayScrimStrong : Theme.bg
        MouseArea {
            anchors.fill: parent
            enabled: root.overlay
            acceptedButtons: Qt.AllButtons
            onWheel: wheel => wheel.accepted = true
        }
    }

    Item {
        id: frame
        anchors.centerIn: parent
        width: root.overlay ? Math.min(parent.width - Metrics.pageMarginPx * 2, Metrics.scaled(760)) : parent.width
        height: root.overlay ? Math.min(parent.height - Metrics.pageMarginPx * 2, Metrics.scaled(820)) : parent.height

        Rectangle {
            anchors.fill: parent
            visible: root.overlay
            radius: Theme.radiusPanel
            color: Theme.bgRaised
            border.width: Theme.hoverBorderWidth
            border.color: Theme.border
        }

        Row {
            id: header
            x: Metrics.pageMarginPx
            y: root.embedded ? 0 : Metrics.pageMarginPx
            spacing: Metrics.scaled(14)
            visible: !root.embedded
            height: visible ? Metrics.touchTargetPx : 0

            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                iconName: root.overlay ? "close" : "arrow_back"
                accessibleName: root.overlay ? "Close" : "Back"
                onClicked: root.back()
            }

            ProviderIcon {
                anchors.verticalCenter: parent.verticalCenter
                width: Metrics.scaled(34)
                height: width
                source: root.provider ? root.provider.iconUrl : ""
                name: root.provider ? root.provider.name : ""
                seed: root.context ? root.context.moduleId : ""
            }

            AppText {
                anchors.verticalCenter: parent.verticalCenter
                text: root.provider ? root.provider.name : ""
                font.pixelSize: Metrics.bodySizePx + Metrics.scaled(2)
                font.weight: Font.DemiBold
            }
        }

        AppText {
            id: compatibilityNotice
            anchors.top: header.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Metrics.pageMarginPx
            anchors.rightMargin: Metrics.pageMarginPx
            visible: Boolean(root.context && root.context.missingHostExtensions
                             && root.context.missingHostExtensions.length > 0)
            height: visible ? implicitHeight + Metrics.scaled(12) : 0
            text: "Update Spool to use all features of this provider."
            font.pixelSize: Metrics.bodySizePx
            color: Theme.textSecondary
            wrapMode: Text.WordWrap
        }

        Loader {
            id: loader
            anchors.top: compatibilityNotice.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.topMargin: root.embedded ? 0 : Metrics.scaled(12)
            focus: true
            asynchronous: true
            Component.onCompleted: root.load()
            onLoaded: InputKeys.focus(item)
            onStatusChanged: if (status === Loader.Error && root.context) {
                                 App.toastMessage("This provider's screen could not be opened")
                                 root.context.close()
                             }
        }

        BusySpinner {
            anchors.centerIn: loader
            width: Metrics.scaled(34)
            height: width
            running: loader.status === Loader.Loading
            visible: running
        }
    }
}
