pragma ComponentBehavior: Bound

import QtQuick
import "../theme"

// An index beside a name-sorted list, as a phone's contacts have: the
// letters evenly down the side, the one the view is in marked, and a large
// bubble naming the letter under the pointer. Pointing, dragging or the
// remote moves through it; letters that do not fit become dots between the
// ones that do, so the column never overlaps itself on a small window.
Item {
    id: root

    // [{label, row, exact}] in list order.
    property var letters: []
    // The letter the view is in, or under remote focus; marked.
    property int currentIndex: -1
    // Remote focus is in the rail.
    property bool active: false
    property bool interactive: true
    readonly property bool pointing: pointer.containsMouse || pointer.pressed
    readonly property bool engaged: active || pointing
    readonly property real restingWidth: Metrics.scaled(24)
    readonly property real letterSize: Metrics.scaled(13)
    readonly property real inset: Metrics.scaled(6)
    readonly property real spacing: letters.length > 0 ? (height - inset * 2) / letters.length : height
    // Every `stride`-th letter is drawn; the rest are dots.
    readonly property int stride: Math.max(1, Math.ceil(letterSize * 1.3 / Math.max(1, spacing)))
    readonly property int hoverIndex: pointing && pointer.mouseY >= 0 ? indexAt(pointer.mouseY) : -1
    // The letter the bubble names: under the pointer, else the remote's.
    readonly property int bubbleIndex: hoverIndex >= 0 ? hoverIndex : active ? currentIndex : -1
    // Chosen by pointer: `final` on release, so dragging previews and lifting
    // commits, the way an index scrubber on a phone does.
    signal letterChosen(int index, bool final)

    width: restingWidth

    function centerOf(index) {
        return inset + (index + 0.5) * spacing
    }

    function indexAt(y) {
        if (letters.length <= 0)
            return -1
        return Math.max(0, Math.min(letters.length - 1, Math.floor((y - inset) / Math.max(1, spacing))))
    }

    // A quiet pill behind the column while it is in use, so it reads as a
    // control rather than as stray letters.
    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: Theme.floatingPanel
        border.width: root.active ? Theme.focusBorderWidth : 1
        border.color: root.active ? Theme.accent : Theme.border
        opacity: root.engaged ? 1 : 0
        visible: opacity > 0

        Behavior on opacity {
            NumberAnimation {
                duration: Theme.reducedMotion ? 0 : 120
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
            readonly property bool hovered: index === root.hoverIndex
            readonly property bool shown: current || hovered || index % root.stride === 0 || index
                                          === root.letters.length - 1

            width: root.width
            height: root.spacing
            y: root.centerOf(index) - height / 2

            Rectangle {
                anchors.centerIn: parent
                width: Math.min(root.width - Metrics.scaled(4), root.letterSize + Metrics.scaled(8))
                height: width
                radius: width / 2
                color: Theme.accent
                visible: letter.current
            }

            AppText {
                anchors.centerIn: parent
                visible: letter.shown
                text: letter.modelData.label
                font.pixelSize: root.letterSize
                font.weight: letter.current || letter.hovered ? Font.Bold : Font.DemiBold
                color: letter.current || letter.hovered || root.engaged ? Theme.textPrimary : Theme.textMuted
                // A position only estimated so far is drawn a little quieter.
                opacity: letter.current || root.engaged || letter.modelData.exact ? 1 : 0.7
            }

            Rectangle {
                anchors.centerIn: parent
                visible: !letter.shown
                width: Metrics.scaled(3)
                height: width
                radius: width / 2
                color: Theme.textMuted
            }
        }
    }

    // The letter under the pointer, large, beside the column.
    Rectangle {
        id: bubble
        readonly property real size: Metrics.scaled(64)
        width: size
        height: size
        radius: Metrics.scaled(14)
        x: -size - Metrics.scaled(12)
        y: root.bubbleIndex >= 0 ? Math.max(0, Math.min(root.height - size, root.centerOf(root.bubbleIndex) - size / 2)) :
                                   0
        color: Theme.floatingPanel
        border.width: Math.max(2, Metrics.scaled(2))
        border.color: Theme.accent
        opacity: root.bubbleIndex >= 0 ? 1 : 0
        visible: opacity > 0
        z: 3

        Behavior on opacity {
            NumberAnimation {
                duration: Theme.reducedMotion ? 0 : 100
            }
        }

        AppText {
            anchors.centerIn: parent
            text: root.bubbleIndex >= 0 && root.bubbleIndex < root.letters.length
                  ? root.letters[root.bubbleIndex].label : ""
            font.pixelSize: Metrics.scaled(32)
            font.weight: Font.Bold
            color: Theme.textPrimary
        }
    }

    MouseArea {
        id: pointer

        property int lastIndex: -1

        anchors.fill: parent
        enabled: root.interactive && root.letters.length > 0
        hoverEnabled: true
        preventStealing: true
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.PointingHandCursor

        function choose(y, final) {
            const index = root.indexAt(y)
            if (index < 0)
                return
            if (index !== lastIndex || final)
                root.letterChosen(index, final)
            lastIndex = index
        }

        onPressed: mouse => {
            lastIndex = -1
            choose(mouse.y, false)
        }
        onPositionChanged: mouse => {
            if (pressed)
                choose(mouse.y, false)
        }
        onReleased: mouse => choose(mouse.y, true)
    }
}
