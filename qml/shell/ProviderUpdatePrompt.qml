import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

// Offered on home when providers have updates and the preference is "Ask
// first". "Always" switches the preference to automatic and updates now.
Item {
    id: root

    property bool shown: false
    property bool dismissed: false
    readonly property var updates: Store.updates
    readonly property bool visibleNow: shown && !dismissed && updates.length > 0 && Settings.value("providers/updates")
                                       === "ask"

    height: visibleNow ? card.height : 0
    visible: visibleNow

    function names() {
        const first = updates[0] ? updates[0].name : ""
        return updates.length > 1 ? first + " and " + (updates.length - 1) + " more" : first
    }

    Surface {
        id: card
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width - Metrics.pageMarginPx * 2, Metrics.scaled(680))
        height: content.implicitHeight + Metrics.scaled(28)
        baseColor: Theme.floatingPanel
        elevated: true

        RowLayout {
            id: content
            anchors.fill: parent
            anchors.leftMargin: Metrics.scaled(18)
            anchors.rightMargin: Metrics.scaled(14)
            spacing: Metrics.scaled(12)

            MaterialIcon {
                name: "system_update"
                iconSize: Metrics.scaled(24)
                iconColor: Theme.accent
            }

            AppText {
                Layout.fillWidth: true
                text: "Updates for " + root.names()
                elide: Text.ElideRight
                maximumLineCount: 1
            }

            ActionButton {
                text: "Not now"
                kind: "flat"
                onClicked: root.dismissed = true
            }
            ActionButton {
                text: "Always"
                kind: "secondary"
                onClicked: {
                    Settings.setValue("providers/updates", "auto")
                    Store.updateAll()
                }
            }
            ActionButton {
                text: "Update"
                kind: "primary"
                onClicked: {
                    Store.updateAll()
                    root.dismissed = true
                }
            }
        }
    }
}
