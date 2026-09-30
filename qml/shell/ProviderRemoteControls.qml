pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Spool

FocusScope {
    id: root
    property var provider
    property string commandKey: "name"
    property string valueKey: "value"
    property string textCapability: "text"
    property string textCommand: ""
    property string messageCommand: ""
    property string mirrorCommand: ""
    property string instructions:
        "These commands act on the selected device, not on this screen. Only commands advertised by that device are shown."
    property var errorMessages: ({})
    property var controls: []
    property var capabilities: ({})
    property bool busy: false
    property string problem: ""
    property int generation: 0
    readonly property string targetId: provider ? String(provider.arguments.targetId || "") : ""
    readonly property bool textAvailable: capabilities[textCapability] === true
    readonly property bool messageAvailable: capabilities.message === true
    function message(reason) {
        const code = String(reason && (reason.code || reason.message) || reason)
        return errorMessages[code] || "Couldn't control this device. Check the connection and refresh."
    }
    function refresh() {
        if (busy)
            return
        const stamp = ++generation
        busy = true
        problem = ""
        provider.request("remoteControls", {
                             targetId: targetId
                         }).then(result => {
                             if (stamp !== generation || provider.closed)
                                 return
                             controls = result.controls || []
                             capabilities = result
                             busy = false
                             Qt.callLater(() => controls.length ? InputKeys.focus(list) : InputKeys.focus(
                                                                      refreshButton))
                         }, reason => {
                             if (stamp === generation) {
                                 busy = false
                                 problem = message(reason)
                             }
                         })
    }
    function send(command, value) {
        if (busy)
            return
        const stamp = ++generation
        const args = {
            targetId: targetId
        }
        args[commandKey] = command
        if (value !== undefined)
            args[valueKey] = value
        busy = true
        problem = ""
        provider.request("remoteControl", args).then(() => {
            if (stamp !== generation || provider.closed)
                return
            busy = false
            if (command === textCommand)
                field.text = ""
        }, reason => {
            if (stamp === generation) {
                busy = false
                problem = message(reason)
            }
        })
    }
    Component.onCompleted: refresh()
    Component.onDestruction: ++generation
    ColumnLayout {
        anchors.fill: parent
        spacing: Metrics.scaled(12)
        SecondaryText {
            Layout.fillWidth: true
            text: root.instructions
            wrapMode: Text.WordWrap
        }
        SecondaryText {
            Layout.fillWidth: true
            visible: !!root.problem
            text: root.problem
            color: Theme.errorText
            wrapMode: Text.WordWrap
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.controls
            keyNavigationEnabled: true
            KeyNavigation.down: field.visible ? field : refreshButton
            delegate: MenuRow {
                required property var modelData
                required property int index
                width: ListView.view.width
                label: modelData.label
                enabled: !root.busy
                highlighted: ListView.isCurrentItem && list.activeFocus
                onHovered: list.currentIndex = index
                onActivated: root.send(modelData.id)
            }
            function activate() {
                if (currentItem && !root.busy)
                    currentItem.activated()
            }
        }
        TextFieldRow {
            id: field
            Layout.fillWidth: true
            visible: root.textAvailable || root.messageAvailable
            enabled: !root.busy
            label: "Text for the device"
            KeyNavigation.up: list
            onAccepted: {
                if (text.length <= 4096)
                    root.send(root.textAvailable ? root.textCommand : root.messageCommand, text)
            }
        }
        ActionButton {
            visible: root.textAvailable
            text: "Send text"
            enabled: !root.busy && field.text.length <= 4096
            onClicked: root.send(root.textCommand, field.text)
        }
        ActionButton {
            visible: root.messageAvailable
            text: "Show message"
            enabled: !root.busy && field.text.length > 0 && field.text.length <= 4096
            onClicked: root.send(root.messageCommand, field.text)
        }
        ActionButton {
            visible: root.capabilities.mirror === true
            text: "Show current item details on player"
            enabled: !root.busy
            onClicked: root.send(root.mirrorCommand)
        }
        ActionButton {
            id: refreshButton
            text: root.busy ? "Working…" : "Refresh device controls"
            enabled: !root.busy
            onClicked: root.refresh()
        }
    }
}
