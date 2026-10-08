import QtQuick
import QtTest
import Spool

TestCase {
    id: testCase
    name: "ProviderForms"
    width: 1000
    height: 800
    visible: true
    when: windowShown
    property var providerContext: provider

    QtObject {
        id: provider
        property bool closed: false
        property var capabilities: ({
                                        discovery: true
                                    })
        property var arguments: ({
                                     kind: "homePin",
                                     title: "Protected account"
                                 })
        property var rows: []
        property var completions: []
        property var pendingAuthentication: null
        property var pendingAuthenticationRejection: null
        property var pendingPoll: null
        property int closes: 0
        function request(operation, args) {
            if (operation === "discover")
                return Promise.resolve({
                                           servers: []
                                       })
            if (operation === "serverCandidates")
                return Promise.resolve({
                                           servers: ["https://fixture.invalid"]
                                       })
            if (operation === "probe")
                return Promise.resolve({
                                           name: "Living room",
                                           server: "https://fixture.invalid",
                                           users: [],
                                           codeEnabled: true
                                       })
            if (operation === "authenticate")
                return new Promise((resolve, reject) => {
                    pendingAuthentication = resolve
                    pendingAuthenticationRejection = reject
                })
            if (operation === "codeStart")
                return Promise.resolve({
                                           code: "123456",
                                           secret: "transient"
                                       })
            if (operation === "codePoll")
                return new Promise(resolve => pendingPoll = resolve)
            return Promise.reject("unexpected_operation")
        }
        function allowOrigin(url) {
            return Promise.resolve({})
        }
        function complete(value) {
            completions = completions.concat([value])
        }
        function cancelLanDiscovery() {
        }
        function close() {
            ++closes
        }
    }
    Component {
        id: login
        ServerLogin {
            provider: testCase.providerContext
            codeLabel: "Link code"
            codeStartOperation: "codeStart"
            codePollOperation: "codePoll"
            codeEnabledField: "codeEnabled"
        }
    }
    Component {
        id: actionPicker
        ProviderActionPicker {
            provider: testCase.providerContext
        }
    }
    Component {
        id: linking
        ProviderLinkScreen {
            provider: testCase.providerContext
            pinRequired: true
        }
    }
    Component {
        id: localFolders
        LocalFolders {
            provider: testCase.providerContext
        }
    }
    function init() {
        provider.completions = []
        provider.pendingAuthentication = null
        provider.pendingAuthenticationRejection = null
        provider.pendingPoll = null
        provider.closes = 0
        provider.arguments = {
            kind: "homePin",
            title: "Protected account"
        }
    }
    function form(component) {
        const result = createTemporaryObject(component, testCase, {
                                                 width: 900,
                                                 height: 700
                                             })
        verify(result)
        return result
    }
    function accessibleItem(item, name) {
        if (!item.Accessible.ignored && item.Accessible.name === name)
            return item
        for (const child of item.children || []) {
            const found = accessibleItem(child, name)
            if (found)
                return found
        }
        return null
    }
    function test_wrappedRejectionKeepsPasswordVisibleWithoutFocusSteal() {
        const view = form(login)
        view.width = 360
        view.height = 280
        view.errorMessages = {
            invalid_credentials:
            "Wrong username or password. Check your account name and enter your password again to sign in to this server."
        }
        view.connect("fixture.invalid")
        tryCompare(view, "step", "account")
        const username = accessibleItem(view, "Username")
        const password = accessibleItem(view, "Password")
        verify(username)
        verify(password)
        username.text = "member"
        password.text = "incorrect"
        password.forceActiveFocus()
        keyClick(Qt.Key_Return)
        tryVerify(() => provider.pendingAuthenticationRejection !== null)
        provider.pendingAuthenticationRejection("invalid_credentials")
        tryCompare(view, "busy", false)
        tryCompare(view, "error", view.errorMessages.invalid_credentials)
        compare(username.text, "member")
        verify(password.activeFocus)
        const error = accessibleItem(view, view.error)
        verify(error)
        compare(error.Accessible.role, Accessible.StaticText)
        compare(error.Accessible.focusable, false)
        const caption = error.children.find(child => typeof child.lineCount === "number" && child.text === view.error)
        verify(caption)
        tryVerify(() => caption.lineCount > 1)
        compare(caption.truncated, false)
        tryVerify(() => {
            for (const item of [password, error]) {
                const bounds = item.mapToItem(view, 0, 0, item.width, item.height)
                if (bounds.x < 0 || bounds.y < 0 || bounds.x + bounds.width > view.width || bounds.y + bounds.height
                        > view.height)
                    return false
            }
            return true
        })
        verify(password.activeFocus)
    }
    function test_destructivePickerStartsOnCancel() {
        provider.arguments = {
            kind: "confirm",
            title: "An item"
        }
        const view = form(actionPicker)
        wait(20)
        view.activate()
        compare(provider.closes, 1)
        compare(provider.completions.length, 0)
    }
    function test_backCancelsPendingPasswordLogin() {
        const view = form(login)
        view.connect("fixture.invalid")
        tryCompare(view, "step", "account")
        view.signIn("member", "password")
        tryVerify(() => provider.pendingAuthentication !== null)
        verify(view.back())
        provider.pendingAuthentication({
                                           account: "stale"
                                       })
        wait(10)
        compare(provider.completions.length, 0)
        compare(view.step, "server")
    }
    function test_cancelledCodeCannotCompleteAccount() {
        const view = form(login)
        view.connect("fixture.invalid")
        tryCompare(view, "step", "account")
        view.startCode()
        tryCompare(view, "code", "123456")
        view.pollCode()
        tryVerify(() => provider.pendingPoll !== null)
        view.cancelCode()
        provider.pendingPoll({
                                 authenticated: true,
                                 account: {
                                     account: "stale"
                                 }
                             })
        wait(10)
        compare(provider.completions.length, 0)
        compare(view.secret, "")
    }
    function test_linkPinClearsBeforeSubmission() {
        const view = form(linking)
        let submitted = ""
        view.pinSubmitted.connect(value => {
            submitted = value
            compare(view.pinText, "")
        })
        view.pinText = "1234"
        view.submitPin()
        compare(submitted, "1234")
    }
    function test_localFoldersAreExplicitAndCombined() {
        const view = form(localFolders)
        compare(view.folders, [])
        view.save()
        compare(provider.completions.length, 0)
        view.addFolder("/media/one")
        view.addFolder("/media/two")
        view.addFolder("/media/one")
        view.save()
        compare(provider.completions[0].configuration.folders, ["/media/one", "/media/two"])
    }
}
