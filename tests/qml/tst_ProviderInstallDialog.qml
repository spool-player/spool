import QtQuick
import QtTest
import "../../qml/shell" as Shell

TestCase {
    id: testCase
    name: "ProviderInstallDialog"
    width: 1280
    height: 720
    visible: true
    when: windowShown
    Component {
        id: dialogComponent
        Shell.ProviderInstallDialog {
            transfers: []
        }
    }
    function test_installLifecycle() {
        const dialog = createTemporaryObject(dialogComponent, testCase)
        verify(dialog)
        verify(!dialog.visible)
        dialog.transfers = [
                    {
                        id: "spool.emby",
                        name: "Emby",
                        state: "downloading",
                        received: 0,
                        total: -1
                    }
                ]
        verify(dialog.visible)
        verify(dialog.routeKey(Qt.Key_Down, "press", false))
        verify(dialog.back())
        verify(dialog.visible)
        dialog.transfers = [
                    {
                        id: "spool.emby",
                        name: "Emby",
                        state: "downloading",
                        received: 512,
                        total: 1024
                    }
                ]
        waitForRendering(dialog)
        dialog.transfers = [
                    {
                        id: "spool.emby",
                        name: "Emby",
                        state: "installing",
                        received: 1024,
                        total: 1024
                    }
                ]
        verify(dialog.visible)
        dialog.transfers = []
        verify(!dialog.visible)
    }
}
