import QtQuick
import QtQuick.Templates as T
import "../theme"

// TV-friendly text input. The wrapper control is the D-pad target; the
// internal TextField only grabs focus (and the virtual keyboard) when the
// user presses Select on the row. Back releases focus and dismisses the IM.
T.Control {
    id: row

    property alias text: field.text
    property alias placeholderText: field.placeholderText
    property alias inputMethodHints: field.inputMethodHints
    property alias echoMode: field.echoMode
    property int enterKeyType: Qt.EnterKeyDefault
    property string label: ""
    property string accessibleName: label.length > 0 ? label : placeholderText
    readonly property bool editing: field.activeFocus
    readonly property bool masked: field.echoMode === TextInput.Password

    // Where there is no on-screen keyboard to defer, the row is a waypoint
    // rather than a stop: anything that focuses it — Tab, D-pad navigation, a
    // restored focus — passes straight through to the field. That removes the
    // state that looks focused, draws a focus ring, and eats every keystroke.
    property bool focusEntersField: Theme.textEntryFollowsFocus

    signal textEdited(string text)
    signal accepted

    // A waypoint must not also be a tab stop, and the field it forwards to has
    // to be one. macOS reports Qt.TabFocusTextControls unless the user turns on
    // Full Keyboard Access, which drops every non-text control from the chain:
    // with the stop on this Control and the field opted out, the whole form
    // became untabbable there. Putting the stop on the field itself is what the
    // waypoint comment above already describes, and it keeps Shift+Tab from
    // bouncing off the row straight back into the field it just left.
    focusPolicy: focusEntersField ? Qt.ClickFocus : Qt.StrongFocus
    implicitHeight: Metrics.scaled(68)
    implicitWidth: Metrics.scaled(400)

    onActiveFocusChanged: if (activeFocus && focusEntersField && !field.activeFocus)
                              InputKeys.focus(field)

    function focusRow() {
        if (focusEntersField) {
            focusField()
            return
        }
        if (field.activeFocus)
            field.focus = false
        InputKeys.focus(row)
    }

    function focusField() {
        InputKeys.focus(field)
        Qt.inputMethod.show()
    }
    function activate() {
        focusField()
    }

    // Only meaningful where the row is a real focus stop. Otherwise there is
    // nothing to step back to, and claiming Back here would trap the user.
    function releaseTextInput() {
        if (!editing || focusEntersField)
            return false
        Qt.inputMethod.hide()
        focusRow()
        return true
    }

    background: Rectangle {
        radius: Theme.radiusMedium
        color: row.editing ? Theme.bgRaised : Theme.bgPanel
        border.width: (row.activeFocus || row.editing) ? Theme.focusBorderWidth : 1
        border.color: (row.activeFocus || row.editing) ? Theme.accent : Theme.border

        SecondaryText {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.leftMargin: Metrics.scaled(16)
            anchors.topMargin: Metrics.scaled(7)
            visible: row.label.length > 0
            text: row.label
            color: Theme.textMuted
            font.pixelSize: Metrics.metaSizePx + Metrics.scaled(2)
            font.weight: Font.Medium
        }
    }

    T.TextField {
        id: field
        Accessible.name: row.accessibleName
        EnterKey.type: row.enterKeyType
        anchors.fill: parent
        anchors.leftMargin: Metrics.scaled(15)
        anchors.rightMargin: Metrics.scaled(15)
        anchors.topMargin: row.label.length > 0 ? Metrics.scaled(22) : Metrics.scaled(9)
        anchors.bottomMargin: Metrics.scaled(9)
        background: Item {}
        color: Theme.textPrimary
        // The UI face has no U+25CF, the mask character Qt asks for by
        // default, so masked text would fall back to a stranger's glyph or a
        // blank box. U+2022 is in the face; bold and spaced it draws the row
        // of dots people expect at these sizes.
        passwordCharacter: "•"
        font.family: Typography.sans
        font.hintingPreference: Typography.sansHinting
        font.preferTypoLineMetrics: true
        font.pixelSize: Metrics.bodySizePx + Metrics.scaled(row.masked ? 6 : 2)
        font.weight: row.masked ? Font.Bold : Font.Normal
        font.letterSpacing: row.masked ? Metrics.scaled(2) : 0
        renderType: Theme.normalTextRenderType
        verticalAlignment: TextInput.AlignVCenter
        selectByMouse: true
        focus: false
        activeFocusOnTab: row.focusEntersField

        onTextEdited: row.textEdited(text)
        onAccepted: row.accepted()

        // Qt's own caret is one pixel wide, which vanishes on a television
        // and is easy to lose on a dense screen. It holds steady while typing
        // and blinks once the text is left alone.
        cursorDelegate: Rectangle {
            id: caret
            property bool lit: true
            width: Math.max(2, Metrics.scaled(2))
            color: Theme.accent
            visible: field.activeFocus && field.selectionStart === field.selectionEnd && lit

            Timer {
                id: blink
                interval: 530
                repeat: true
                running: field.activeFocus
                onRunningChanged: caret.lit = true
                onTriggered: caret.lit = !caret.lit
            }

            Connections {
                target: field
                function onCursorPositionChanged() {
                    caret.lit = true
                    blink.restart()
                }
            }
        }

        // Templates carry the placeholder text but draw nothing for it, and a
        // hint is the difference between a labelled box and a guess.
        SecondaryText {
            anchors.fill: parent
            visible: field.displayText.length === 0 && field.placeholderText.length > 0
            text: field.placeholderText
            color: Theme.textDisabled
            font.pixelSize: Metrics.bodySizePx + Metrics.scaled(2)
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: !row.focusEntersField
        onClicked: row.focusField()
        propagateComposedEvents: true
    }
}
