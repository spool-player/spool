pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes
import "../theme"
import "LetterScale.js" as LetterScale

// A long list's scroll bar, which for a name-sorted list is also its index.
// At rest it is a slim bar; while the list moves, its thumb carries a callout
// naming where the view is. In a name-sorted list every letter owns a
// stretch of the track (see LetterScale.js), so pointing at the bar names the
// letter under the pointer, a tap jumps to that letter's first title and a
// drag scrubs through the list. With remote focus on the bar, the callout
// shows the letters either side of the current one, which Up and Down step
// to.
Item {
    id: root

    required property Flickable flickable
    // [{label, row}] in list order; empty for a list not sorted by name.
    property var letters: []
    // How rows lie in the flickable: `columns` to a line, `rowHeight` apart.
    property int columns: 1
    property real rowHeight: 0
    // Where the view is, shown while the list moves.
    property string label: ""
    // Lists only a screen or two long have no need of the callout.
    property bool labelEnabled: true
    // Remote focus is on the bar, at letter `activeIndex`.
    property bool active: false
    property int activeIndex: -1
    property bool interactive: true
    property real minimumSize: 0.04

    readonly property int visualWidth: Math.max(Metrics.scaled(10), 10)
    // A pointer resting on the bar, and still the thing in use: a key press
    // means a pointer that vanished without leaving no longer counts.
    readonly property bool hovering: pointer.containsMouse && !pointer.pressed && Metrics.pointerActive &&
                                     !Metrics.coarsePointer

    readonly property bool engaged: active || pointer.pressed || hovering
    readonly property real scrollRange: Math.max(0, flickable.contentHeight - flickable.height)
    readonly property real handleHeight: Math.max(height * minimumSize, flickable.contentHeight > 0 ? height * Math.min(
                                                                                                          1, flickable.height
                                                                                                          / flickable.contentHeight) :
                                                                                                      height)
    readonly property real availableTrack: Math.max(0, height - handleHeight)
    readonly property var letterMap: {
        if (letters.length < 2 || scrollRange <= 0 || rowHeight <= 0 || availableTrack <= 0)
            return null
        const lineOf = row => Math.floor(row / Math.max(1, columns))
        return LetterScale.scale(letters.map(letter => lineOf(letter.row) * rowHeight / scrollRange), Metrics.scaled(
                                     16) / availableTrack)
    }
    readonly property real handleY: availableTrack * LetterScale.toTrack(scrollRange > 0 ? (flickable.contentY
                                                                                            - flickable.originY)
                                                                                           / scrollRange : 0, letterMap)
    readonly property int pointerIndex: letterMap && (pointer.pressed || hovering) ? LetterScale.indexAt(trackFraction(
                                                                                                             pointer.mouseY),
                                                                                                         letterMap) : -1
    // The letter the callout names: the remote's, else the one pointed at.
    readonly property int calloutIndex: active ? activeIndex : pointerIndex
    readonly property string calloutText: calloutIndex >= 0 && calloutIndex < letters.length ? String(
                                                                                                   letters[calloutIndex].label) :
                                                                                               pointer.pressed
                                                                                               || feedback.running
                                                                                               ? label : ""

    // The view moved because of the bar.
    signal scrolled
    // A tap on the track named this letter.
    signal letterChosen(int index)

    // Show the callout for a moment, as when the list moves.
    function reveal() {
        if (labelEnabled)
            feedback.restart()
    }

    function trackFraction(y) {
        return availableTrack > 0 ? LetterScale.clamp((y - handleHeight / 2) / availableTrack) : 0
    }

    visible: scrollRange > 0
    Accessible.role: Accessible.ScrollBar
    Accessible.name: label

    Connections {
        target: root.flickable
        function onContentYChanged() {
            root.reveal()
        }
    }

    Timer {
        id: feedback
        interval: 700
    }

    Rectangle {
        id: track
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: root.engaged ? root.visualWidth + Metrics.scaled(4) : root.visualWidth
        radius: Math.max(1, Math.round(width * 0.22))
        color: Theme.accentPanel
        opacity: root.engaged ? 0.9 : 0.55

        Behavior on width {
            NumberAnimation {
                duration: Theme.reducedMotion ? 0 : 90
            }
        }
        Behavior on opacity {
            NumberAnimation {
                duration: Theme.reducedMotion ? 0 : 90
            }
        }
    }

    Rectangle {
        id: handle
        width: root.active ? track.width : Math.max(Metrics.scaled(6), 6)
        height: root.handleHeight
        x: root.width - (track.width + width) / 2
        y: root.handleY
        radius: Math.max(1, Math.round(width * 0.22))
        color: Theme.accent
        border.width: root.active ? Math.max(1, Metrics.scaled(2)) : 0
        border.color: Theme.textPrimary
    }

    // Rounded on its left, top and bottom, with no right edge: its shoulders
    // funnel into the bar, so it reads as coming from it.
    Shape {
        id: callout

        readonly property bool wheel: root.active && root.letters.length > 1
        readonly property real stroke: Math.max(2, Metrics.scaled(2))
        readonly property real inset: stroke / 2
        readonly property real cornerRadius: Metrics.scaled(12)
        readonly property real neckWidth: Metrics.scaled(16)
        readonly property real mouthHalf: Metrics.scaled(13)
        readonly property real bodyRight: width - neckWidth
        readonly property real midY: height / 2
        readonly property real anchorY: root.active || !(pointer.pressed || root.hovering) ? root.handleY
                                                                                             + root.handleHeight / 2 :
                                                                                             pointer.mouseY

        x: track.x - width
        y: Math.max(0, Math.min(root.height - height, anchorY - height / 2))
        width: Metrics.scaled(52) + neckWidth
        height: Metrics.scaled(48) + (wheel ? Metrics.scaled(44) : 0)
        opacity: root.labelEnabled && root.calloutText.length > 0 && (root.engaged || feedback.running) ? 1 : 0
        visible: opacity > 0
        preferredRendererType: Shape.CurveRenderer

        Behavior on opacity {
            NumberAnimation {
                duration: Theme.reducedMotion ? 0 : 100
            }
        }

        // The stroke runs mouth -> top -> left -> bottom -> mouth and stops
        // there; only the fill closes the contour across the bar.
        ShapePath {
            fillColor: Theme.accentPanel
            strokeColor: Theme.accent
            strokeWidth: callout.stroke
            capStyle: ShapePath.FlatCap
            joinStyle: ShapePath.RoundJoin
            startX: callout.width
            startY: callout.midY - callout.mouthHalf

            PathCubic {
                control1X: callout.width - callout.neckWidth * 0.35
                control1Y: callout.midY - callout.mouthHalf
                control2X: callout.bodyRight + callout.neckWidth * 0.55
                control2Y: callout.inset
                x: callout.bodyRight
                y: callout.inset
            }
            PathLine {
                x: callout.cornerRadius + callout.inset
                y: callout.inset
            }
            PathArc {
                radiusX: callout.cornerRadius
                radiusY: callout.cornerRadius
                direction: PathArc.Counterclockwise
                x: callout.inset
                y: callout.cornerRadius + callout.inset
            }
            PathLine {
                x: callout.inset
                y: callout.height - callout.cornerRadius - callout.inset
            }
            PathArc {
                radiusX: callout.cornerRadius
                radiusY: callout.cornerRadius
                direction: PathArc.Counterclockwise
                x: callout.cornerRadius + callout.inset
                y: callout.height - callout.inset
            }
            PathLine {
                x: callout.bodyRight
                y: callout.height - callout.inset
            }
            PathCubic {
                control1X: callout.bodyRight + callout.neckWidth * 0.55
                control1Y: callout.height - callout.inset
                control2X: callout.width - callout.neckWidth * 0.35
                control2Y: callout.midY + callout.mouthHalf
                x: callout.width
                y: callout.midY + callout.mouthHalf
            }
        }

        // From the remote, the letters Up and Down go to, quietly either side.
        Column {
            anchors.verticalCenter: parent.verticalCenter
            x: (callout.bodyRight - width) / 2
            width: callout.bodyRight - Metrics.scaled(8)

            AppText {
                readonly property int index: root.activeIndex - 1
                width: parent.width
                height: Metrics.scaled(22)
                visible: callout.wheel
                text: index >= 0 && index < root.letters.length ? String(root.letters[index].label) : ""
                font.pixelSize: Metrics.scaled(15)
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                color: Theme.textMuted
            }
            AppText {
                width: parent.width
                height: Metrics.scaled(40)
                text: root.calloutText
                font.pixelSize: Metrics.scaled(26)
                fontSizeMode: Text.Fit
                minimumPixelSize: Metrics.scaled(8)
                elide: Text.ElideRight
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                color: Theme.textPrimary
            }
            AppText {
                readonly property int index: root.activeIndex + 1
                width: parent.width
                height: Metrics.scaled(22)
                visible: callout.wheel
                text: index >= 0 && index < root.letters.length ? String(root.letters[index].label) : ""
                font.pixelSize: Metrics.scaled(15)
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                color: Theme.textMuted
            }
        }
    }

    // One grab for the whole strip. Pressing the thumb drags it from where it
    // was held. In an index, a tap on the track jumps to the letter there and
    // moving first turns it into a drag with the thumb under the pointer;
    // otherwise pressing the track moves the thumb there at once. An exclusive
    // grab on press keeps the delegate MouseArea under the bar from taking it.
    MouseArea {
        id: pointer

        property real grabOffset: 0
        property real pressY: 0
        property bool scrubbing: false

        function scrubTo(y) {
            if (root.availableTrack <= 0)
                return
            const before = root.flickable.contentY
            root.flickable.contentY = root.flickable.originY + LetterScale.fromTrack((y - grabOffset)
                                                                                     / root.availableTrack,
                                                                                     root.letterMap) * root.scrollRange
            if (root.flickable.contentY !== before)
                root.scrolled()
        }

        anchors.fill: parent
        enabled: root.interactive
        hoverEnabled: true
        preventStealing: true
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.PointingHandCursor

        onPressed: mouse => {
            pressY = mouse.y
            const onHandle = mouse.y >= root.handleY && mouse.y <= root.handleY + root.handleHeight
            grabOffset = onHandle ? mouse.y - root.handleY : root.handleHeight / 2
            scrubbing = onHandle || !root.letterMap
            if (!onHandle && !root.letterMap)
                scrubTo(mouse.y)
        }
        onPositionChanged: mouse => {
            // Moving over the bar again after keys is the pointer back in use.
            if (!pressed && !Metrics.coarsePointer)
                Metrics.pointerActive = true
            if (!pressed)
                return
            if (!scrubbing && Math.abs(mouse.y - pressY) > Metrics.scaled(6))
                scrubbing = true
            if (scrubbing)
                scrubTo(mouse.y)
        }
        onReleased: mouse => {
            if (!scrubbing && root.letterMap)
                root.letterChosen(LetterScale.indexAt(root.trackFraction(mouse.y), root.letterMap))
            scrubbing = false
            root.reveal()
        }
        onCanceled: scrubbing = false
    }
}
