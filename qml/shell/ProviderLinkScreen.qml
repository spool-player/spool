pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Spool

FocusScope {
    id: root
    property var provider
    property string title: ""
    property string instructions: ""
    property string code: ""
    property string error: ""
    property bool busy: false
    property bool pinRequired: false
    property string pinLabel: "PIN"
    property alias pinText: pin.text
    property var choices: []
    property bool retryVisible: true
    property string backText: "Back"
    signal retryRequested
    signal backRequested
    signal choiceSelected(int index)
    signal pinSubmitted(string value)
    function focusChoices() {
        InputKeys.focus(choicesList)
    }
    function focusPin() {
        pin.focusRow()
    }
    function submitPin() {
        const value = pin.text
        pin.text = ""
        pinSubmitted(value)
    }
    function activate() {
        const item = Window.activeFocusItem
        if (item && typeof item.activate === "function")
            item.activate()
        else if (item && typeof item.clicked === "function")
            item.clicked()
        else if (pin.activeFocus)
            submitPin()
    }
    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - Metrics.pageMarginPx * 2, Metrics.scaled(620))
        height: Math.min(implicitHeight, parent.height - Metrics.pageMarginPx * 2)
        spacing: Metrics.scaled(12)
        ProviderCompatibilityNotice {
            Layout.fillWidth: true
            provider: root.provider
        }
        AppText {
            Layout.fillWidth: true
            text: root.title
            font.pixelSize: Metrics.titleSizePx
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        SecondaryText {
            Layout.fillWidth: true
            visible: !root.code && !!root.instructions
            text: root.instructions
            wrapMode: Text.WordWrap
        }
        ProviderCodePanel {
            Layout.fillWidth: true
            visible: !!root.code
            code: root.code
            instructions: root.instructions
        }
        ListView {
            id: choicesList
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: Math.min(contentHeight, root.height * 0.45)
            visible: root.choices.length > 0
            clip: true
            model: root.choices
            keyNavigationEnabled: true
            spacing: Metrics.scaled(8)
            delegate: ServerCard {
                required property var modelData
                required property int index
                width: ListView.view.width
                title: modelData.title || modelData.name || ""
                serverAddress: modelData.address || ""
                enabled: !root.busy
                onAccepted: root.choiceSelected(index)
            }
            function activate() {
                if (currentItem)
                    currentItem.accepted()
            }
        }
        TextFieldRow {
            id: pin
            Layout.fillWidth: true
            visible: root.pinRequired
            label: root.pinLabel
            echoMode: TextInput.Password
            inputMethodHints: Qt.ImhDigitsOnly | Qt.ImhNoPredictiveText
            onAccepted: root.submitPin()
        }
        ActionButton {
            Layout.fillWidth: true
            visible: root.pinRequired
            text: "Unlock"
            kind: "primary"
            enabled: !root.busy
            onClicked: root.submitPin()
        }
        BusySpinner {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: Metrics.scaled(24)
            Layout.preferredHeight: Metrics.scaled(24)
            running: root.busy
            visible: running
        }
        SecondaryText {
            Layout.fillWidth: true
            visible: !!root.error
            text: root.error
            color: Theme.errorText
            wrapMode: Text.WordWrap
        }
        ActionButton {
            visible: root.retryVisible
            text: "Get a new code"
            enabled: !root.busy
            onClicked: root.retryRequested()
        }
        ActionButton {
            visible: !!root.backText
            text: root.backText
            kind: "flat"
            onClicked: root.backRequested()
        }
    }
}
