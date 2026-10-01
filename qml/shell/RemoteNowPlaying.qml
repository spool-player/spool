pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

FocusScope {
    id: root
    property var remote: RemoteTargets
    property var shell
    signal openControls
    readonly property var snapshot: remote.state || ({})
    readonly property var commands: snapshot.commands || []
    readonly property bool failed: remote.problem.length > 0
    readonly property string toggleAction: snapshot.state === "paused" ? "unpause" : "pause"
    readonly property bool toggleAvailable: remote.selectedTargetId.length > 0 && commands.indexOf(toggleAction) >= 0
    property int actionIndex: 0
    implicitHeight: Math.max(Metrics.touchTargetPx, Metrics.scaled(64))
    function activate() {
        if (actionIndex === 1 && toggleAvailable)
            remote.send({
                            action: toggleAction
                        })
        else
            openControls()
    }
    function back() {
        if (shell)
            shell.focusContent()
        return true
    }
    function routeKey(key, phase, repeat) {
        if (!InputKeys.isDirection(key))
            return false
        if (phase === "release")
            return true
        if (key === Qt.Key_Left)
            actionIndex = 0
        else if (key === Qt.Key_Right)
            actionIndex = toggleAvailable ? 1 : 0
        else if (shell)
            shell.focusContent()
        return true
    }
    Rectangle {
        anchors.fill: parent
        color: Theme.bgRaised
    }
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Metrics.pageMarginPx
        anchors.rightMargin: Metrics.pageMarginPx
        spacing: Metrics.gapPx
        Image {
            source: root.snapshot.artwork || ""
            visible: source.toString().length > 0
            Layout.preferredWidth: root.height - Metrics.scaled(12)
            Layout.preferredHeight: Layout.preferredWidth
            fillMode: Image.PreserveAspectFit
            asynchronous: true
        }
        MenuRow {
            Layout.fillWidth: true
            label: root.failed ? root.remote.problem : root.snapshot.title || root.remote.selectedTarget.name
                                 || "Remote playback"
            detail: root.failed ? "Choose a device or explicitly return to local playback." : (
                                      root.remote.selectedTarget.name || "") + " · " + (root.snapshot.state
                                                                                        || "Connecting")
            iconName: root.failed ? "warning" : "cast"
            highlighted: root.activeFocus && root.actionIndex === 0
            compact: true
            onActivated: root.openControls()
            Accessible.name: "Open remote playback controls: " + label
        }
        IconButton {
            visible: root.toggleAvailable
            iconName: root.toggleAction === "pause" ? "pause" : "play_arrow"
            accessibleName: root.toggleAction === "pause" ? "Pause remote playback" : "Resume remote playback"
            selected: root.activeFocus && root.actionIndex === 1
            enabled: !root.remote.busy
            onClicked: root.remote.send({
                                            action: root.toggleAction
                                        })
        }
    }
}
