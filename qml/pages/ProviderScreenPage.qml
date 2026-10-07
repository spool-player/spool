import QtQuick
import "../shell"

// The route for a provider's sign-in or settings screen.
ProviderSurface {
    id: page

    property var shell

    context: shell ? shell.routeArgs.context : null
    onFinished: (result, cancelled) => {
        if (!cancelled && context && context.role === "login")
            // Setup admission already opens its actionable profile tile.
            return
        // Closing settings or cancelling login goes back to its originating page.
        if (Router.route === "providerScreen" && Router.canPop)
            Router.pop("accounts")
    }
}
