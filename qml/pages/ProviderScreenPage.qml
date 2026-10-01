import QtQuick
import "../shell"
import "../primitives"
import "../theme"

// The route for a provider's sign-in or settings screen.
ProviderSurface {
    id: page

    property var shell

    context: shell ? shell.routeArgs.context : null
    property bool completing: false
    property string completionError: ""
    onFinished: (result, cancelled) => {
        if (!cancelled && context && context.role === "login") {
            completing = true
            return
        }
        // Signing in lands on home through Providers.accountAdded; anything
        // else goes back to where it was opened from.
        if (Router.route === "providerScreen" && Router.canPop)
            Router.pop("accounts")
    }
    Connections {
        target: Providers
        function onProblem(message) {
            if (page.completing)
                page.completionError = message
        }
    }
    Rectangle {
        anchors.fill: parent
        visible: page.completing
        color: Theme.bg
        Column {
            anchors.centerIn: parent
            spacing: Metrics.scaled(20)
            BusySpinner {
                anchors.horizontalCenter: parent.horizontalCenter
                width: Metrics.scaled(36)
                height: width
                running: page.completing && !page.completionError
            }
            AppText {
                text: page.completionError || "Opening your library…"
            }
            ActionButton {
                visible: page.completionError.length > 0
                text: "Back"
                onClicked: Router.pop("accounts")
            }
        }
    }
}
