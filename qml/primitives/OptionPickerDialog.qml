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
    readonly property real panelHeight: Math.min(height - edgeMargin * 2, Math.max(rowHeight + metrics.scaled(16), Math.min(
                                                                                       options.length, 8) * rowHeight
                                                                                   + metrics.scaled(16)))

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
            if (phase === "release")
                dismissed()
            return true
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

    MouseArea {
        anchors.fill: parent
        enabled: root.placementReady
        onClicked: root.dismissed()
    }

    PopupMenuPanel {
        id: menuPanel
        objectName: "optionPickerPanel"
        width: root.panelWidth
        open: root.visible && root.placementReady
        openHeight: root.panelHeight
        baseColor: Theme.floatingPanel

        MenuListView {
            id: optionList
            objectName: "optionPickerList"
            anchors.fill: parent
            anchors.margins: root.metrics.scaled(8)
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
                highlighted: optionList.activeFocus && optionList.currentIndex === index
                onHovered: optionList.currentIndex = index
                onActivated: root.selected(index)
            }
        }
    }
}
