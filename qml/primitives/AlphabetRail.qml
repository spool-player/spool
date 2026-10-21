pragma ComponentBehavior: Bound

import QtQuick
import "../theme"

// The letters of a name-sorted list, beside its scroll bar. At rest each
// letter sits where its titles are in the list, so the bar doubles as a map
// of the library. Pointed at, or entered from the remote, the rail opens into
// an evenly spaced index with the letters nearest the pointer magnified, so a
// letter with three titles is as easy to hit as one with three thousand.
Item {
    id: root

    required property Flickable flickable
    // [{label, row, exact}] in list order.
    property var letters: []
    property int columns: 1
    property real cellHeight: 1
    // The letter under the view or under remote focus; highlighted.
    property int currentIndex: -1
    // Remote focus is in the rail.
    property bool active: false
    property bool interactive: true
    readonly property bool pointing: pointer.containsMouse || pointer.pressed
    readonly property bool expanded: active || pointing
    readonly property real restingWidth: Metrics.scaled(24)
    readonly property real expandedWidth: Metrics.scaled(64)
    readonly property real letterSize: Metrics.scaled(15)
    readonly property real spacing: letters.length > 0 ? height / letters.length : height
    property real pointerY: -1
    // Chosen by pointer: `final` on release, so dragging previews and lifting
    // commits, the way an index scrubber on a phone does.
    signal letterChosen(int index, bool final)

    width: restingWidth

    function restingCenter(row) {
        const range = Math.max(0, flickable.contentHeight - flickable.height)
        const offset = Math.floor(Math.max(0, row) / Math.max(1, columns)) * cellHeight
        const fraction = range > 0 ? Math.min(1, offset / range) : 0
        return letterSize / 2 + fraction * Math.max(0, height - letterSize)
    }

    function expandedCenter(index) {
        return (index + 0.5) * spacing
    }

    function indexAt(y) {
        if (letters.length <= 0)
            return -1
        return Math.max(0, Math.min(letters.length - 1, Math.floor(y / Math.max(1, spacing))))
    }

    // At rest, letters crowded together by a small library or a lopsided
    // alphabet give way to the ones before them; the current one always shows.
    readonly property var restingVisible: {
        const shown = []
        let lastY = -Infinity
        for (let i = 0; i < letters.length; ++i) {
            const y = restingCenter(letters[i].row)
            const keep = y - lastY >= letterSize * 1.15
            shown.push(keep)
            if (keep)
                lastY = y
        }
        return shown
    }

    // Backdrop of the open index, reaching out over the grid.
    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.expandedWidth
        radius: Theme.radiusMedium
        color: Theme.floatingPanel
        border.width: 1
        border.color: root.active ? Theme.accent : Theme.border
        opacity: root.expanded ? 1 : 0
        visible: opacity > 0

        Behavior on opacity {
            NumberAnimation {
                duration: Theme.reducedMotion ? 0 : 140
            }
        }
    }

    Repeater {
        model: root.letters

        delegate: Item {
            id: letter

            required property int index
            required property var modelData
            readonly property bool current: index === root.currentIndex
            readonly property real centerY: root.expanded ? root.expandedCenter(index) : root.restingCenter(
                                                                modelData.row)

            // Magnified by closeness to the pointer, dock-style.
            readonly property real lens: {
                if (!root.pointing || root.pointerY < 0)
                    return current && root.active ? 1.6 : 1
                const distance = (centerY - root.pointerY) / Math.max(1, root.spacing * 1.8)
                return 1 + 1.1 * Math.exp(-distance * distance)
            }

            width: root.restingWidth
            height: root.letterSize
            x: root.width - width - (root.expanded ? (lens - 1) * Metrics.scaled(16) + Metrics.scaled(18) : 0)
            y: centerY - height / 2
            visible: root.expanded || root.restingVisible[index] || current
            z: current ? 2 : 1

            Behavior on y {
                enabled: !Theme.reducedMotion
                NumberAnimation {
                    duration: 160
                    easing.type: Easing.OutCubic
                }
            }
            Behavior on x {
                enabled: !Theme.reducedMotion
                NumberAnimation {
                    duration: 90
                }
            }

            Rectangle {
                anchors.centerIn: parent
                width: Math.max(parent.height, label.implicitWidth + Metrics.scaled(8)) * letter.lens
                height: parent.height * letter.lens + Metrics.scaled(4)
                radius: height / 2
                color: Theme.accent
                visible: letter.current && root.expanded
            }

            AppText {
                id: label
                anchors.centerIn: parent
                text: letter.modelData.label
                scale: letter.lens
                font.pixelSize: root.letterSize
                font.weight: letter.current ? Font.Bold : Font.DemiBold
                // An estimated position is drawn a little quieter than one
                // the server or a loaded row has confirmed.
                color: letter.current && !root.expanded ? Theme.accent : root.expanded ? Theme.textPrimary :
                                                                                         Theme.textMuted
                opacity: root.expanded || letter.modelData.exact || letter.current ? 1 : 0.6
            }
        }
    }

    MouseArea {
        id: pointer

        property int lastIndex: -1

        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.expanded ? root.expandedWidth : root.restingWidth
        enabled: root.interactive && root.letters.length > 0
        hoverEnabled: true
        preventStealing: true
        acceptedButtons: Qt.LeftButton

        function choose(y, final) {
            const index = root.indexAt(y)
            if (index < 0)
                return
            if (index !== lastIndex || final)
                root.letterChosen(index, final)
            lastIndex = index
        }

        onPositionChanged: mouse => {
            root.pointerY = mouse.y
            if (pressed)
                choose(mouse.y, false)
        }
        onPressed: mouse => {
            lastIndex = -1
            root.pointerY = mouse.y
            choose(mouse.y, false)
        }
        onReleased: mouse => choose(mouse.y, true)
        onExited: root.pointerY = -1
    }
}
