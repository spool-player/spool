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
    signal finished

    focus: true

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
        target: root.context
        function onFinished() {
            root.finished()
        }
    }

    Rectangle {
        anchors.fill: parent
        color: root.overlay ? Theme.overlayScrimStrong : Theme.bg
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
            y: Metrics.pageMarginPx
            spacing: Metrics.scaled(14)
            height: Metrics.touchTargetPx

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

        Loader {
            id: loader
            anchors.top: header.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.topMargin: Metrics.scaled(12)
            focus: true
            asynchronous: true
            Component.onCompleted: {
                if (root.context)
                    setSource(root.context.component, {
                                  "provider": root.context
                              })
            }
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
