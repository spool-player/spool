import QtQuick
import "../shell"

// The route for a provider's sign-in or settings screen.
ProviderSurface {
    id: page

    property var shell

    context: shell ? shell.routeArgs.context : null
    onFinished: {
        // Signing in lands on home through Providers.accountAdded; anything
        // else goes back to where it was opened from.
        if (Router.route === "providerScreen" && Router.canPop)
            Router.pop("accounts")
    }
}
