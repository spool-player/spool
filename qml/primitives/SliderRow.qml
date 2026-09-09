import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T
import "../theme"

SettingRow {
    id: root

    property int value: 0
    property int from: 0
    property int to: 100
    property int step: 1
    property string unitText: ""
    property bool selected: false
    property bool logarithmic: false

    signal valueEdited(real value)
    signal valuePreviewed(real value)
    signal interactionStarted

    // The page drives selection and left/right adjustment for the whole list,
    // so the row itself never takes focus; that keeps one focus owner instead
    // of juggling the row, a text field and a slider.
    focus: false
    focusPolicy: Qt.NoFocus
    pointerActivationEnabled: false
    valueTextVisible: false
    rowFocus: selected

    function clamp(v) {
        return Math.max(from, Math.min(to, Number(v)))
    }

    function rounded(v) {
        if (logarithmic)
            return clamp(valueFromSlider(sliderPosition(Number(v || 0))))
        const safeStep = Math.max(1, Number(step || 1))
        return clamp(Math.round(Number(v || 0) / safeStep) * safeStep)
    }
    function sliderPosition(v) {
        return logarithmic ? Math.log(clamp(v)) / Math.LN2 : clamp(v)
    }

    function valueFromSlider(v) {
        return logarithmic ? Math.pow(2, Math.round(v)) : v
    }

    function formatValue(v) {
        return String(Math.round(Number(v)))
    }

    function commit(next) {
        const value = rounded(next)
        valueField.text = formatValue(value)
        valueEdited(value)
    }

    function preview(next) {
        const value = rounded(next)
        valueField.text = formatValue(value)
        valuePreviewed(value)
    }

    onValueChanged: {
        const formatted = formatValue(value)
        if (valueField.text !== formatted)
            valueField.text = formatted
    }

    // Reserve the widest reading the range can produce so the digits stay in
    // one column while the slider is dragged.
    TextMetrics {
        id: widest
        font: valueField.font
        text: formatValue(from).length >= formatValue(to).length ? formatValue(from) : formatValue(to)
    }

    trailing: [
        InlineSlider {
            Layout.preferredWidth: Math.max(Metrics.scaled(160), Math.min(Metrics.scaled(320), root.width * 0.3))
            Layout.alignment: Qt.AlignVCenter
            highlighted: root.rowFocus
            from: root.sliderPosition(root.from)
            to: root.sliderPosition(root.to)
            stepSize: root.logarithmic ? 1 : root.step
            value: root.sliderPosition(root.value)
            barHeight: Metrics.scaled(7)
            handleSize: Metrics.scaled(18)
            minimumInteractionHeight: Math.max(48, Metrics.touchTargetPx)
            onMoved: newValue => {
                root.interactionStarted()
                root.preview(root.valueFromSlider(newValue))
            }
            onCommitted: newValue => root.commit(root.valueFromSlider(newValue))
        },
        Rectangle {
            readonly property int pad: Metrics.scaled(12)

            Layout.preferredWidth: pad * 2 + widest.width + (unitLabel.visible ? unitLabel.width + Metrics.scaled(5) :
                                                                                 0)

            Layout.preferredHeight: Math.max(Metrics.scaled(34), Math.round(Metrics.controlHeightPx * 0.78))
            Layout.alignment: Qt.AlignVCenter
            radius: Theme.radiusMedium
            color: Theme.bgRaised
            border.width: valueField.activeFocus ? Theme.focusBorderWidth : Theme.hoverBorderWidth
            border.color: valueField.activeFocus ? Theme.accent : Theme.border
            antialiasing: true

            AppText {
                id: unitLabel
                visible: root.unitText.length > 0
                anchors.right: parent.right
                anchors.rightMargin: parent.pad
                anchors.baseline: valueField.baseline
                text: root.unitText
                color: Theme.textSecondary
                font.pixelSize: Metrics.metaSizePx
                font.weight: Font.DemiBold
            }

            T.TextField {
                id: valueField
                anchors.right: unitLabel.visible ? unitLabel.left : parent.right
                anchors.rightMargin: unitLabel.visible ? Metrics.scaled(5) : parent.pad
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: widest.width
                text: root.formatValue(root.value)
                // Typing an exact value is worth having on a desktop; on a TV
                // it means summoning an on-screen keyboard to do what the
                // D-pad already does.
                readOnly: Platform.isTV
                horizontalAlignment: TextInput.AlignRight
                verticalAlignment: TextInput.AlignVCenter
                // Not ImhDigitsOnly: audio sync takes negative values, so the
                // minus key has to stay reachable.
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                validator: IntValidator {
                    bottom: root.from
                    top: root.to
                }
                color: Theme.textPrimary
                selectedTextColor: Theme.textPrimary
                selectionColor: Theme.accentDim
                font.family: Typography.sans
                font.hintingPreference: Typography.sansHinting
                font.pixelSize: Metrics.metaSizePx + 1
                font.weight: Font.DemiBold
                onActiveFocusChanged: if (activeFocus)
                                          root.interactionStarted()
                onAccepted: root.commit(text)
                onEditingFinished: root.commit(text)
            }
        }
    ]
}
