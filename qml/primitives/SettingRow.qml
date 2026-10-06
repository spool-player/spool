import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T
import "../theme"

T.Control {
    id: root
    property string title: ""
    property string description: ""
    property string valueText: ""
    property bool valueTextVisible: true
    property bool rowFocus: activeFocus
    property bool pointerActivationEnabled: true
    // Controls placed where valueText would otherwise go, laid out rather than
    // anchored so the title column cannot run underneath them.
    property alias trailing: trailingRow.data
    signal clicked
    focusPolicy: Qt.StrongFocus
    focus: true
    implicitHeight: Math.max(Metrics.scaled(68), implicitContentHeight + Metrics.scaled(28))
    leftPadding: Metrics.scaled(14)
    rightPadding: Metrics.scaled(14)
    topPadding: Metrics.scaled(14)
    bottomPadding: Metrics.scaled(14)
    Accessible.role: Accessible.Button
    Accessible.name: title
    Accessible.description: description + (valueTextVisible && valueText.length ? ". " + valueText : "")
    Accessible.focused: rowFocus
    Accessible.onPressAction: if (enabled && pointerActivationEnabled)
                                  clicked()

    background: Surface {
        focused: root.rowFocus
        hovered: hover.hovered
    }

    HoverHandler {
        id: hover
    }
    TapHandler {
        enabled: root.pointerActivationEnabled
        onTapped: {
            if (root.focusPolicy !== Qt.NoFocus)
                InputKeys.focus(root)
            root.clicked()
        }
    }

    contentItem: RowLayout {
        spacing: Metrics.scaled(18)

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Metrics.scaled(3)
            AppText {
                Layout.fillWidth: true
                text: root.title
                font.pixelSize: Metrics.bodySizePx
                font.weight: Font.Medium
                maximumLineCount: 1
                elide: Text.ElideRight
            }
            AppText {
                text: root.description
                visible: text.length > 0
                color: Theme.textMuted
                font.pixelSize: Metrics.metaSizePx
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }

        SecondaryText {
            visible: root.valueTextVisible
            text: root.valueText
            color: Theme.textSecondary
            font.pixelSize: Metrics.metaSizePx
            Layout.maximumWidth: Math.max(Metrics.scaled(96), root.width * 0.42)
            maximumLineCount: 1
            elide: Text.ElideRight
        }

        RowLayout {
            id: trailingRow
            spacing: Metrics.scaled(12)
            Layout.alignment: Qt.AlignVCenter
        }
    }
}
