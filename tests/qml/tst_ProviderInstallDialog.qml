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
        // A store install or a background update shows on its own tile, if
        // anywhere; it never takes over the screen.
        dialog.transfers = [
                    {
                        id: "spool.emby",
                        name: "Emby",
                        state: "downloading",
                        received: 0,
                        total: -1
                    }
                ]
        verify(!dialog.visible)
        // An install the viewer started from a file waits on this dialog.
        dialog.transfers = [
                    {
                        id: "spool.emby",
                        name: "Emby",
                        state: "downloading",
                        operationToken: "file-1",
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
                        operationToken: "file-1",
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
                        operationToken: "file-1",
                        received: 1024,
                        total: 1024
                    }
                ]
        verify(dialog.visible)
        dialog.transfers = []
        verify(!dialog.visible)
    }
}
