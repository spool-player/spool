pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

// Which providers fill Home, as a multi-select popover beside whatever opened
// it. Every provider is a switch and the popover stays open while they are
// flipped. Home needs one provider that can fill it, so the last such switch
// is held on and says why rather than silently refusing.
FocusScope {
    id: root

    property Item anchorItem: null
    property bool placementReady: false
    readonly property var choices: Home.providerChoices
    readonly property var entries: [
        {
            "all": true
        }
    ].concat(choices)
    readonly property bool allShown: Home.hiddenProviderIds.length === 0
    readonly property real edgeMargin: Metrics.scaled(12)
    readonly property real rowHeight: Math.max(Metrics.touchTargetPx, Metrics.scaled(64))
    readonly property real panelPadding: Metrics.scaled(8)
    readonly property real panelWidth: Math.min(width - edgeMargin * 2, Metrics.scaled(520))
    readonly property real panelHeight: Math.min(height - edgeMargin * 2, header.implicitHeight + entries.length
                                                 * rowHeight + panelPadding * 3)

    signal dismissed

    anchors.fill: parent
    focus: true
    z: 200

    function detail(entry) {
        if (entry.all)
            return allShown ? "Every provider is on" : "Turn every provider back on"
        if (entry.required)
            return "Home needs at least one provider"
        const profiles = Number(entry.profiles || 0)
        if (profiles <= 0)
            return "No signed-in profile"
        return profiles === 1 ? "1 profile" : profiles + " profiles"
    }

    function choose(index) {
        const entry = entries[index]
        if (!entry)
            return
        if (entry.all)
            Home.showAllProviders()
        else if (!entry.required)
            Home.setProviderShown(String(entry.id), !entry.shown)
    }

    function present() {
        placementReady = false
        if (!visible || width <= 0 || height <= 0)
            return
        Qt.callLater(function () {
            if (!root.anchorItem) {
                panel.x = (root.width - root.panelWidth) / 2
                panel.y = (root.height - root.panelHeight) / 2
            } else {
                const anchor = root.anchorItem.mapToItem(root, 0, 0)
                const gap = Metrics.scaled(6)
                const below = anchor.y + root.anchorItem.height + gap
                const above = anchor.y - root.panelHeight - gap
                const fitsBelow = below + root.panelHeight <= root.height - root.edgeMargin
                panel.x = Math.max(root.edgeMargin, Math.min(root.width - root.panelWidth - root.edgeMargin, anchor.x
                                                             + root.anchorItem.width - root.panelWidth))
                panel.y = Math.max(root.edgeMargin, Math.min(root.height - root.panelHeight - root.edgeMargin, fitsBelow
                                                             || above < root.edgeMargin ? below : above))
            }
            root.placementReady = true
            list.currentIndex = Math.max(0, list.currentIndex)
            InputKeys.focus(list)
        })
    }

    function routeKey(key, phase, repeat) {
        if (InputKeys.isBack(key, false, false)) {
            if (phase === "press" && !repeat)
                dismissed()
            return phase === "press"
        }
        if (InputKeys.isDirection(key)) {
            if (phase === "press" && key === Qt.Key_Up)
                list.moveSelection(-1)
            else if (phase === "press" && key === Qt.Key_Down)
                list.moveSelection(1)
            return true
        }
        return InputKeys.isAccept(key)
    }

    function activate() {
        list.activate()
    }

    function back() {
        dismissed()
        return true
    }

    Component.onCompleted: present()
    onVisibleChanged: present()
    onWidthChanged: present()
    onHeightChanged: present()

    MouseArea {
        anchors.fill: parent
        enabled: root.placementReady
        onClicked: root.dismissed()
    }

    // A cheap lift off the page; a blurred shadow costs too much on a TV.
    Rectangle {
        x: panel.x - Metrics.scaled(2)
        y: panel.y + Metrics.scaled(6)
        width: panel.width + Metrics.scaled(4)
        height: panel.height
        radius: panel.radius + Metrics.scaled(2)
        color: "#66000000"
        visible: panel.visible
        opacity: panel.opacity
    }

    PopupMenuPanel {
        id: panel
        objectName: "homeProviderPicker"
        width: root.panelWidth
        open: root.visible && root.placementReady
        openHeight: root.panelHeight
        baseColor: Theme.floatingPanel
        border.color: Theme.borderStrong
        opacity: open ? 1 : 0

        Behavior on opacity {
            enabled: !Theme.reducedMotion
            NumberAnimation {
                duration: 110
                easing.type: Easing.OutCubic
            }
        }

        ColumnLayout {
            id: header
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: Metrics.scaled(18)
            anchors.rightMargin: Metrics.scaled(18)
            anchors.topMargin: Metrics.scaled(16)
            spacing: Metrics.scaled(4)

            AppText {
                Layout.fillWidth: true
                text: "Providers on Home"
                font.pixelSize: Metrics.bodySizePx + Metrics.scaled(2)
                font.weight: Font.DemiBold
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }
            SecondaryText {
                Layout.fillWidth: true
                Layout.bottomMargin: Metrics.scaled(6)
                text: "Search and playback still use every provider."
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: Theme.border
            }
        }

        MenuListView {
            id: list
            objectName: "homeProviderPickerList"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: header.bottom
            anchors.bottom: parent.bottom
            anchors.margins: root.panelPadding
            // A count, not the array: rows then outlive a toggle, so the
            // switch animates and the selection stays where it was.
            model: root.entries.length
            dismissOnHorizontal: false
            entryProvider: index => root.entries[index]
            onAccepted: index => root.choose(index)
            onDismissed: root.dismissed()

            delegate: Item {
                id: row

                required property int index
                readonly property var modelData: root.entries[index] || ({})
                readonly property bool highlighted: list.activeFocus && list.currentIndex === index
                readonly property bool on: modelData.all ? root.allShown : Boolean(modelData.shown)

                width: list.width
                height: root.rowHeight
                Accessible.role: modelData.all ? Accessible.Button : Accessible.CheckBox
                Accessible.name: modelData.all ? "All providers" : String(modelData.name)
                Accessible.description: root.detail(modelData)
                Accessible.checkable: !modelData.all
                Accessible.checked: on
                Accessible.onPressAction: root.choose(index)
                Accessible.onToggleAction: root.choose(index)

                Rectangle {
                    anchors.fill: parent
                    anchors.topMargin: Metrics.scaled(2)
                    anchors.bottomMargin: Metrics.scaled(2)
                    radius: Theme.radiusLarge
                    color: row.highlighted ? Theme.focusedFill : rowHover.hovered ? Theme.bgHover : "transparent"
                    border.width: row.highlighted ? Theme.focusBorderWidth : 0
                    border.color: Theme.accent
                    antialiasing: true
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Metrics.scaled(12)
                    anchors.rightMargin: Metrics.scaled(14)
                    spacing: Metrics.scaled(14)

                    Rectangle {
                        visible: row.modelData.all
                        Layout.preferredWidth: Metrics.scaled(36)
                        Layout.preferredHeight: Metrics.scaled(36)
                        radius: Math.round(width * 0.22)
                        color: Theme.accentPanel
                        MaterialIcon {
                            anchors.centerIn: parent
                            name: "apps"
                            iconSize: Metrics.scaled(22)
                            iconColor: Theme.accent
                        }
                    }
                    ProviderIcon {
                        visible: !row.modelData.all
                        Layout.preferredWidth: Metrics.scaled(36)
                        Layout.preferredHeight: Metrics.scaled(36)
                        source: row.modelData.iconUrl || ""
                        name: String(row.modelData.name || "")
                        seed: String(row.modelData.id || "")
                        opacity: row.on ? 1 : 0.55
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Metrics.scaled(2)
                        AppText {
                            Layout.fillWidth: true
                            text: row.modelData.all ? "All providers" : String(row.modelData.name)
                            color: row.on || row.highlighted ? Theme.textPrimary : Theme.textSecondary
                            font.pixelSize: Metrics.bodySizePx
                            font.weight: Font.Medium
                            maximumLineCount: 1
                            elide: Text.ElideRight
                        }
                        SecondaryText {
                            Layout.fillWidth: true
                            text: root.detail(row.modelData)
                            color: Theme.textMuted
                            font.pixelSize: Metrics.metaSizePx
                            maximumLineCount: 1
                            elide: Text.ElideRight
                        }
                    }
                    MaterialIcon {
                        visible: row.modelData.all
                        Layout.preferredWidth: Metrics.scaled(30)
                        name: "check"
                        iconSize: Metrics.scaled(24)
                        iconColor: Theme.accent
                        opacity: root.allShown ? 1 : 0
                    }
                    SwitchIndicator {
                        visible: !row.modelData.all
                        Layout.alignment: Qt.AlignVCenter
                        checked: row.on
                        focused: row.highlighted
                        animate: true
                        opacity: row.modelData.required ? 0.6 : 1
                    }
                }

                HoverHandler {
                    id: rowHover
                    onHoveredChanged: if (hovered)
                                          list.currentIndex = row.index
                }
                TapHandler {
                    gesturePolicy: TapHandler.DragThreshold
                    onTapped: {
                        list.currentIndex = row.index
                        root.choose(row.index)
                    }
                }
            }
        }
    }
}
