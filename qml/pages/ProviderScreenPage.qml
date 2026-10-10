import QtQuick
import QtQuick.Layouts
import "../shell"
import "../theme"
import "../primitives"

// Login, private approval and account settings stay provider-owned.
FocusScope {
    id: page
    property var shell
    readonly property var context: shell ? shell.routeArgs.context : null
    readonly property var installedModule: context ? Providers.modules.find(module => module.id === context.moduleId) :
                                                     null

    focus: true

    function routeKey(key, phase, repeat) {
        return surface.routeKey(key, phase, repeat)
    }
    function activate() {
        return surface.activate()
    }
    function back() {
        return surface.back()
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }
    RowLayout {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Metrics.pageMarginPx
        spacing: Metrics.scaled(14)
        IconButton {
            iconName: "arrow_back"
            accessibleName: "Back"
            onClicked: page.back()
        }
        ProviderIcon {
            Layout.preferredWidth: Metrics.scaled(34)
            Layout.preferredHeight: width
            source: page.installedModule ? page.installedModule.iconUrl : ""
            name: page.installedModule ? page.installedModule.name : ""
            seed: page.context ? page.context.moduleId : ""
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Metrics.scaled(2)
            AppText {
                Layout.fillWidth: true
                text: page.installedModule ? page.installedModule.name : "Provider"
                font.pixelSize: Metrics.bodySizePx + Metrics.scaled(2)
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            SecondaryText {
                Layout.fillWidth: true
                text: page.installedModule ? "Installed provider version " + page.installedModule.version : ""
                elide: Text.ElideRight
            }
        }
    }
    ProviderSurface {
        id: surface
        anchors.top: header.bottom
        anchors.topMargin: Metrics.scaled(12)
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        embedded: true
        context: page.context
        onFinished: (result, cancelled) => {
            if (!cancelled && context && context.role === "login")
                // Setup admission already opens its actionable profile tile.
                return
            if (Router.route === "providerScreen" && Router.canPop)
                Router.pop("accounts")
        }
    }
}
