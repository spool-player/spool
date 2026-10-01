pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import Spool

// A sign-in code to type on another device. Instructions sit below its box;
// the address can be followed wherever there is a browser, and the code can
// be copied wherever there is a clipboard.
ColumnLayout {
    id: root
    property string code: ""
    property string instructions: ""
    property string linkUrl: ""
    property bool copied: false
    readonly property string linkText: linkUrl.replace(/^https?:\/\//i, "").replace(/\/+$/, "")
    // A television has neither a browser worth sending someone to nor a
    // clipboard; it only needs the address spelled out.
    readonly property bool interactive: !Platform.isTV

    onCodeChanged: copied = false
    spacing: Metrics.scaled(14)

    AppText {
        Layout.fillWidth: true
        visible: !!root.linkUrl
        text: root.linkText
        elide: Text.ElideRight
        color: Theme.accent
        font.pixelSize: Metrics.scaled(30)
        font.weight: Font.DemiBold
        font.underline: root.interactive && linkHover.hovered
        HoverHandler {
            id: linkHover
            enabled: root.interactive
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            enabled: root.interactive
            onTapped: Qt.openUrlExternally(root.linkUrl)
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: Metrics.scaled(6)
        spacing: Metrics.scaled(12)

        Surface {
            Layout.fillWidth: true
            implicitHeight: codeText.implicitHeight + Metrics.scaled(36)
            baseColor: Theme.accentPanel
            AppText {
                id: codeText
                anchors.centerIn: parent
                text: root.code
                font.pixelSize: Metrics.scaled(56)
                font.weight: Font.Bold
                font.letterSpacing: Metrics.scaled(14)
            }
        }

        ActionButton {
            Layout.alignment: Qt.AlignVCenter
            visible: root.interactive
            iconName: root.copied ? "check" : "content_copy"
            text: root.copied ? "Copied" : "Copy"
            onClicked: {
                clipboard.copyText(root.code)
                root.copied = true
            }
        }
    }
    AppText {
        Layout.fillWidth: true
        visible: !!root.instructions
        text: root.instructions
        wrapMode: Text.WordWrap
        color: Theme.textSecondary
        font.pixelSize: Metrics.scaled(22)
        lineHeight: 1.15
    }

    // Qt Quick has no clipboard of its own; a text editor's does.
    TextEdit {
        id: clipboard
        visible: false
        function copyText(value) {
            text = value
            selectAll()
            copy()
            text = ""
        }
    }
}
