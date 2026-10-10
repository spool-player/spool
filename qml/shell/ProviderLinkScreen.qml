pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Spool

FocusScope {
    id: root
    property var provider
    readonly property var installedModule: provider ? Providers.modules.find(module => module.id === provider.moduleId) :
                                                      null
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
    // Where the code is entered, such as https://plex.tv/link. A desktop
    // opens it once a code is ready; a phone offers it as a link.
    property string linkUrl: ""
    // Small print, such as trademark notices, kept apart from the steps.
    property string footnote: ""
    property bool linkOpened: false
    readonly property bool desktop: !Platform.isTV && !Platform.isAndroid
    onCodeChanged: {
        if (code && linkUrl && desktop && !linkOpened) {
            linkOpened = true
            Qt.openUrlExternally(linkUrl)
        }
    }
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
        width: Math.min(parent.width - Metrics.pageMarginPx * 2, Metrics.scaled(720))
        height: Math.min(implicitHeight, parent.height - Metrics.pageMarginPx * 2)
        spacing: Metrics.scaled(16)
        AppText {
            Layout.fillWidth: true
            Layout.bottomMargin: Metrics.scaled(4)
            text: root.title
            font.pixelSize: Metrics.scaled(40)
            font.weight: Font.DemiBold
            wrapMode: Text.WordWrap
        }
        AppText {
            Layout.fillWidth: true
            visible: !root.code && !!root.instructions
            text: root.instructions
            color: Theme.textSecondary
            font.pixelSize: Metrics.scaled(20)
            wrapMode: Text.WordWrap
        }
        ProviderCodePanel {
            Layout.fillWidth: true
            visible: !!root.code
            code: root.code
            instructions: root.linkUrl ? (root.instructions || "On your phone or computer, go to") : root.instructions
            linkUrl: root.linkUrl
        }
        RowLayout {
            Layout.fillWidth: true
            visible: !!root.code && !root.error
            spacing: Metrics.scaled(10)
            BusySpinner {
                Layout.preferredWidth: Metrics.scaled(20)
                Layout.preferredHeight: Metrics.scaled(20)
                running: parent.visible
            }
            SecondaryText {
                Layout.fillWidth: true
                text: "Waiting for you to enter the code…"
                font.pixelSize: Metrics.bodySizePx
                wrapMode: Text.WordWrap
            }
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
                providerName: root.installedModule ? root.installedModule.name : ""
                providerId: root.provider ? root.provider.moduleId : ""
                providerIcon: root.installedModule ? root.installedModule.iconUrl : ""
                providerVersion: root.installedModule ? root.installedModule.version : ""
                enabled: !root.busy
                onAccepted: {
                    choicesList.currentIndex = index
                    choicesList.forceActiveFocus()
                    root.choiceSelected(index)
                }
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
        Flow {
            Layout.fillWidth: true
            Layout.topMargin: Metrics.scaled(8)
            spacing: Metrics.scaled(12)
            ActionButton {
                visible: !!root.code && !!root.linkUrl && root.desktop
                kind: "primary"
                iconName: "open_in_new"
                text: "Open " + root.linkUrl.replace(/^https?:\/\//i, "").replace(/\/+$/, "")
                onClicked: Qt.openUrlExternally(root.linkUrl)
            }
            ActionButton {
                visible: root.retryVisible
                iconName: "refresh"
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
        SecondaryText {
            Layout.fillWidth: true
            Layout.topMargin: Metrics.scaled(12)
            visible: !!root.footnote
            text: root.footnote
            color: Theme.textMuted
            font.pixelSize: Metrics.metaSizePx
            wrapMode: Text.WordWrap
        }
    }
}
