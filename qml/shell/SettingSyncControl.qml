import QtQuick
import QtQuick.Templates as T
import "../theme"
import "../primitives"

T.Control {
    id: root

    required property string settingKey
    required property var syncState
    required property bool actionFocused
    property string settingTitle: settingKey
    property string sourceLabel: ""
    property bool helpFocused: false
    readonly property bool eligible: Boolean(syncState && syncState.eligible)
    readonly property bool syncEnabled: Boolean(syncState && syncState.enabled)
    readonly property string status: syncState ? String(syncState.status || "unsupported") : "unsupported"
    readonly property bool confirmedSynced: eligible && syncEnabled && status === "synced"
    readonly property bool localOnly: !syncEnabled || status === "off"
    readonly property string iconName: syncState && syncState.backend === "native" ? "sync" : syncState
                                                                                     && syncState.backend === "spool"
                                                                                     ? "cloud_sync" : "sync_disabled"
    readonly property color statusColor: confirmedSynced ? Theme.success : status === "pending" || status === "saving"
                                                           ? Theme.pending : status === "error" ? Theme.errorText :
                                                                                                  Theme.textMuted
    readonly property string statusLabel: localOnly ? "Local only" : status === "synced" ? "Synced" : status
                                                                                           === "pending" ? "Pending" :
                                                                                                           status
                                                                                                           === "saving"
                                                                                                           ? "Saving" :
                                                                                                             status
                                                                                                             === "offline"
                                                                                                             ? "Offline" :
                                                                                                               status
                                                                                                               === "error"
                                                                                                               ? "Error" :
                                                                                                                 "Unavailable"
    readonly property string helpText: {
        if (syncState && syncState.help)
            return String(syncState.help)
        const channel = syncState && syncState.backend === "native" ? "Changes the service's own preference." :
                                                                      syncState && syncState.backend === "spool"
                                                                      ? "Spool-specific sync. Does not change the service's settings." :
                                                                        "This account does not support synchronizing this setting."
        return channel + (sourceLabel.length ? " " + sourceLabel + "." : "") + " " + statusLabel + "."
    }
    readonly property bool helpVisible: visible && (hovered || actionFocused || helpFocused)

    signal toggled

    visible: eligible
    enabled: eligible
    focusPolicy: Qt.NoFocus
    activeFocusOnTab: false
    hoverEnabled: true
    implicitWidth: Math.max(Metrics.controlHeightPx, statusText.implicitWidth + Metrics.scaled(20))
    implicitHeight: Math.max(Metrics.touchTargetPx, Metrics.scaled(66))

    Accessible.role: Accessible.CheckBox
    Accessible.name: "Sync " + settingTitle
    Accessible.description: helpText
    Accessible.checkable: true
    Accessible.checked: syncEnabled
    Accessible.focused: actionFocused
    Accessible.onToggleAction: root.toggled()
    Accessible.onPressAction: root.toggled()

    background: Rectangle {
        radius: Theme.radiusMedium
        color: root.actionFocused ? Theme.accentPanel : "transparent"
        border.width: root.actionFocused ? Theme.focusBorderWidth : 0
        border.color: Theme.accent
    }

    contentItem: Column {
        spacing: Metrics.scaled(3)
        Item {
            width: parent.width
            height: Math.max(Metrics.iconSizePx, Metrics.scaled(26))
            MaterialIcon {
                id: channelIcon
                anchors.centerIn: parent
                name: root.iconName
                iconSize: Math.max(Metrics.iconSizePx, Metrics.scaled(26))
                iconColor: root.statusColor
                Rectangle {
                    visible: root.localOnly
                    anchors.centerIn: parent
                    width: parent.width + Metrics.scaled(3)
                    height: Math.max(2, Metrics.scaled(2))
                    rotation: -45
                    color: root.statusColor
                }
            }
            Rectangle {
                objectName: "syncStatusDot"
                anchors.left: channelIcon.right
                anchors.bottom: channelIcon.bottom
                width: Metrics.scaled(8)
                height: width
                radius: width / 2
                color: root.confirmedSynced ? Theme.success : "transparent"
                border.width: Math.max(1, Metrics.scaled(1))
                border.color: root.statusColor
            }
        }
        AppText {
            id: statusText
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.statusLabel
            color: root.statusColor
            font.pixelSize: Metrics.metaSizePx
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: root.toggled()
    }

    T.ToolTip {
        id: help
        objectName: "syncHelp"
        parent: root
        visible: root.helpVisible
        x: root.width - width
        y: root.height + Metrics.scaled(6)
        padding: Metrics.scaled(16)
        implicitWidth: Math.min(Metrics.scaled(520), Math.max(Metrics.scaled(260), (root.Window.window
                                                                                    ? root.Window.window.width :
                                                                                      Metrics.scaled(560))
                                                              - Metrics.scaled(40)))
        implicitHeight: contentItem.implicitHeight + topPadding + bottomPadding
        closePolicy: T.Popup.NoAutoClose
        contentItem: AppText {
            text: root.helpText
            color: Theme.textPrimary
            font.pixelSize: Metrics.bodySizePx
            wrapMode: Text.Wrap
        }
        background: Rectangle {
            radius: Theme.radiusMedium
            color: Theme.floatingPanel
            border.color: Theme.borderStrong
            border.width: Math.max(1, Metrics.scaled(1))
        }
    }
}
