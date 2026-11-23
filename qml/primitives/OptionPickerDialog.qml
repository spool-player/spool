pragma ComponentBehavior: Bound

import QtQuick
import "../theme"

FocusScope {
    id: root

    property string title: "Choose an option"
    property var options: []
    property int currentIndex: 0
    property Item anchorItem: null
    property bool spaceRequested: false
    property bool placementReady: false
    property var inputKeys: InputKeys
    property var metrics: Metrics
    readonly property real edgeMargin: metrics.scaled(12)
    readonly property real rowHeight: Math.max(metrics.scaled(44), metrics.controlHeightPx)
    readonly property real panelWidth: Math.min(width - edgeMargin * 2, Math.max(metrics.scaled(280), Math.min(metrics.scaled(
                                                                                                                   680), anchorItem
                                                                                                               ? anchorItem.width
                                                                                                                 * 0.78 : metrics.scaled(
                                                                                                                     460))))
    readonly property real listPadding: metrics.scaled(6)
    readonly property real headerHeight: title.length > 0 ? titleLabel.implicitHeight + metrics.scaled(22) : 0
    readonly property real panelHeight: Math.min(height - edgeMargin * 2, headerHeight + Math.max(1, Math.min(options.length,
                                                                                                              8)) * rowHeight
                                                 + listPadding * 2)
    readonly property bool scrolls: options.length * rowHeight > optionList.height + 1

    signal selected(int index)
    signal dismissed
    signal spaceBelowRequired(real pixels)

    anchors.fill: parent
    focus: true
    z: 100

    function focusCurrent() {
        const count = optionList.count
        optionList.currentIndex = count > 0 ? Math.max(0, Math.min(root.currentIndex, count - 1)) : -1
        if (optionList.currentIndex >= 0)
            optionList.positionViewAtIndex(optionList.currentIndex, ListView.Contain)
        inputKeys.focus(optionList)
    }

    function schedulePresentation() {
        placementReady = false
        if (!visible || !anchorItem)
            return
        spaceRequested = false
        Qt.callLater(completePresentation)
    }

    function completePresentation() {
        if (!visible || !anchorItem)
            return false
        if (!positionPopup())
            return false
        placementReady = true
        focusCurrent()
        return true
    }

    function positionPopup() {
        if (!anchorItem || width <= 0 || height <= 0)
            return false

        const anchor = anchorItem.mapToItem(root, 0, 0)
        const below = anchor.y + anchorItem.height + metrics.scaled(6)
        const deficit = below + panelHeight + edgeMargin - height
        if (deficit > 1 && !spaceRequested) {
            spaceRequested = true
            spaceBelowRequired(deficit)
            // A host may scroll to make room, but hosts without a scroll
            // handler still need the clamped/above-anchor placement below.
            Qt.callLater(completePresentation)
            return false
        }

        const above = anchor.y - panelHeight - metrics.scaled(6)
        const desiredY = below + panelHeight <= height - edgeMargin || above < edgeMargin ? below : above
        menuPanel.x = Math.max(edgeMargin, Math.min(width - panelWidth - edgeMargin, anchor.x + anchorItem.width
                                                    - panelWidth))
        menuPanel.y = Math.max(edgeMargin, Math.min(height - panelHeight - edgeMargin, desiredY))
        return true
    }

    function routeKey(key, phase, repeat) {
        if (inputKeys.isBack(key, false, false)) {
            if (phase === "press" && !repeat)
                dismissed()
            // KeyRouter swallows the release of a claimed Back press. An
            // isolated release can fall through to back() instead.
            return phase === "press"
        }
        if (inputKeys.isDirection(key)) {
            if (phase === "press" && key === Qt.Key_Up)
                optionList.moveSelection(-1)
            else if (phase === "press" && key === Qt.Key_Down)
                optionList.moveSelection(1)
            return true
        }
        return inputKeys.isAccept(key)
    }

    function activate() {
        optionList.activate()
    }

    function back() {
        dismissed()
        return true
    }

    Component.onCompleted: schedulePresentation()
    onVisibleChanged: schedulePresentation()
    onAnchorItemChanged: schedulePresentation()
    onWidthChanged: schedulePresentation()
    onHeightChanged: schedulePresentation()

    MouseArea {
        anchors.fill: parent
        enabled: root.placementReady
        onClicked: root.dismissed()
    }

    // A cheap lift off the page; a blurred shadow costs too much on a TV.
    Rectangle {
        x: menuPanel.x - root.metrics.scaled(2)
        y: menuPanel.y + root.metrics.scaled(6)
        width: menuPanel.width + root.metrics.scaled(4)
        height: menuPanel.height
        radius: menuPanel.radius + root.metrics.scaled(2)
        color: "#66000000"
        visible: menuPanel.visible
        opacity: menuPanel.opacity
    }

    PopupMenuPanel {
        id: menuPanel
        objectName: "optionPickerPanel"
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

        AppText {
            id: titleLabel
            visible: root.title.length > 0
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: root.metrics.scaled(16)
            anchors.rightMargin: root.metrics.scaled(16)
            anchors.topMargin: root.metrics.scaled(12)
            text: root.title
            color: Theme.textMuted
            font.pixelSize: root.metrics.metaSizePx
            font.weight: Font.DemiBold
            maximumLineCount: 1
            elide: Text.ElideRight
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }

        Rectangle {
            visible: titleLabel.visible
            anchors.left: parent.left
            anchors.right: parent.right
            y: root.headerHeight - 1
            height: 1
            color: Theme.border
        }

        MenuListView {
            id: optionList
            objectName: "optionPickerList"
            anchors.fill: parent
            anchors.topMargin: root.headerHeight + root.listPadding
            anchors.bottomMargin: root.listPadding
            anchors.leftMargin: root.listPadding
            anchors.rightMargin: root.listPadding + (root.scrolls ? scrollBar.visualWidth + root.metrics.scaled(6) : 0)
            model: root.options
            currentIndex: root.currentIndex
            onDismissed: root.dismissed()
            onAccepted: index => root.selected(index)

            delegate: MenuRow {
                required property int index
                required property var modelData
                width: optionList.width
                rowHeight: root.rowHeight
                minimumRowHeight: root.metrics.controlHeightPx
                label: String(modelData)
                checked: index === root.currentIndex
                checkIconName: "check"
                highlighted: optionList.activeFocus && optionList.currentIndex === index
                onHovered: optionList.currentIndex = index
                onActivated: root.selected(index)
            }
        }

        ListScrollBar {
            id: scrollBar
            visible: root.scrolls
            anchors.top: optionList.top
            anchors.bottom: optionList.bottom
            anchors.right: parent.right
            flickable: optionList
        }
    }
}
