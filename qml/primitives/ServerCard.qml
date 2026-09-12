import QtQuick
import QtQuick.Layouts
import "../theme"

// One server, as a list row. Discovered servers, the address being typed and
// the server already chosen all use this, so the trailing column has to carry
// a live connection attempt as well as a settled result; `tone` colours it.
FocusScope {
    id: root

    property string title: ""
    property string serverAddress: ""
    property string detail: ""
    property string status: ""
    // neutral | pending | positive | negative
    property string tone: "neutral"
    property bool selectable: true
    property bool focused: activeFocus
    // Rows inside a list well take their frame from the well. A row standing
    // on its own is a panel and has to draw one.
    property bool inset: false

    readonly property color toneColor: {
        switch (tone) {
        case "positive":
            return Theme.online
        case "pending":
            return Theme.pending
        case "negative":
            return Theme.errorText
        default:
            return Theme.textSecondary
        }
    }

    signal accepted

    implicitHeight: Math.max(Metrics.touchTargetPx, Metrics.scaled(detail.length > 0 ? 82 : 64))
    focusPolicy: Qt.StrongFocus

    Rectangle {
        anchors.fill: parent
        anchors.margins: root.inset ? Metrics.scaled(3) : 0
        radius: Theme.radiusMedium
        color: root.focused ? Theme.accentPanel : hover.hovered && Metrics.pointerActive ? Theme.bgHover : root.inset
                                                                                           ? "transparent" :
                                                                                             Theme.bgRaised
        border.width: root.focused ? Theme.focusBorderWidth : root.inset ? 0 : 1
        border.color: root.focused ? Theme.accent : Theme.border
        opacity: root.selectable ? 1 : 0.62
        antialiasing: true
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Metrics.scaled(16)
        anchors.rightMargin: Metrics.scaled(14)
        spacing: Metrics.scaled(14)

        Item {
            Layout.preferredWidth: Metrics.scaled(24)
            Layout.preferredHeight: Metrics.scaled(24)
            Layout.alignment: Qt.AlignVCenter

            MaterialIcon {
                anchors.centerIn: parent
                visible: root.tone !== "pending"
                name: root.tone === "negative" ? "error_outline" : "dns"
                iconSize: Metrics.scaled(22)
                iconColor: root.focused ? Theme.accent : root.tone === "negative" ? Theme.errorText : Theme.textMuted
            }

            // Reaching a server is the one thing on this screen that takes
            // long enough to need saying so while it happens.
            BusySpinner {
                anchors.centerIn: parent
                width: Metrics.scaled(18)
                height: width
                visible: root.tone === "pending"
                color: Theme.pending
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: Metrics.scaled(2)

            AppText {
                Layout.fillWidth: true
                text: root.title
                font.pixelSize: Metrics.bodySizePx + Metrics.scaled(2)
                font.weight: Font.Medium
                maximumLineCount: 1
                elide: Text.ElideRight
            }

            SecondaryText {
                Layout.fillWidth: true
                visible: root.serverAddress.length > 0
                text: root.serverAddress
                color: Theme.textMuted
                font.pixelSize: Metrics.metaSizePx
                maximumLineCount: 1
                elide: Text.ElideRight
            }

            SecondaryText {
                Layout.fillWidth: true
                visible: root.detail.length > 0
                text: root.detail
                color: root.tone === "negative" ? Theme.errorText : Theme.textMuted
                font.pixelSize: Metrics.metaSizePx
                maximumLineCount: 1
                elide: Text.ElideRight
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignVCenter
            visible: root.status.length > 0
            spacing: Metrics.scaled(7)

            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: Metrics.scaled(6)
                Layout.preferredHeight: Metrics.scaled(6)
                radius: width / 2
                visible: root.tone === "positive"
                color: root.toneColor
                antialiasing: true
            }

            AppText {
                text: root.status
                color: root.tone === "positive" ? Theme.textSecondary : root.toneColor
                font.pixelSize: Metrics.metaSizePx
                font.weight: Font.Medium
                maximumLineCount: 1
            }
        }

        MaterialIcon {
            Layout.alignment: Qt.AlignVCenter
            visible: root.selectable
            name: "chevron_right"
            iconSize: Metrics.scaled(20)
            iconColor: root.focused ? Theme.accent : Theme.textDisabled
        }
    }

    TapHandler {
        enabled: root.selectable
        onTapped: root.accepted()
    }

    HoverHandler {
        id: hover
    }
}
