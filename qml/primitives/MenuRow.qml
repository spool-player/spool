import QtQuick
import QtQuick.Layouts
import "../theme"

Item {
    id: root

    Component.onCompleted: InputLatency.noteDelegate("menu_row", 1)
    Component.onDestruction: InputLatency.noteDelegate("menu_row", -1)

    property string label: ""
    property string detail: ""
    property string iconName: ""
    property bool checked: false
    property bool section: false
    property bool highlighted: false
    property bool actionable: true
    property bool pointerActivationEnabled: true
    // Rows are sized for a remote by default. A pointer opens the same menu as
    // a dropdown beside its button, where that scale reads as oversized and
    // costs the width the labels need.
    property bool compact: false
    property int rowHeight: Math.max(Metrics.touchTargetPx, Metrics.scaled(detail.length > 0 ? (compact ? 44 : 54) : (compact
                                                                                                                      ? 36 : 46)))
    property int minimumRowHeight: Metrics.touchTargetPx
    property string checkIconName: "done"
    property bool stepperVisible: false
    property bool stepperEnabled: true
    property string stepperText: ""
    property string stepperEditText: stepperText
    property bool stepperEditable: false
    property bool stepperInvalid: false

    signal activated
    signal hovered
    signal decreaseRequested
    signal increaseRequested
    signal stepperAccepted(string text)

    width: parent ? parent.width : Metrics.scaled(320)
    // Keep padding inside each delegate: adjacent rows share an edge, not a
    // dead strip. Explicit row heights must still respect the touch minimum.
    height: section ? Metrics.scaled(34) : Math.max(minimumRowHeight, rowHeight)

    AppText {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        visible: root.section
        text: root.label
        color: Theme.textMuted
        font.pixelSize: Metrics.metaSizePx
        font.weight: Font.DemiBold
        maximumLineCount: 1
        elide: Text.ElideRight
    }

    Rectangle {
        anchors.fill: parent
        anchors.topMargin: Metrics.scaled(1)
        anchors.bottomMargin: Metrics.scaled(1)
        visible: !root.section
        radius: Theme.radiusSmall
        color: root.highlighted ? Theme.focusedFill : hover.hovered && root.actionable ? Theme.bgHover : root.checked
                                                                                         ? Theme.accentPanel :
                                                                                           "transparent"
        border.width: root.highlighted ? Theme.hoverBorderWidth : 0
        border.color: Theme.accent
        antialiasing: true
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Metrics.scaled(10)
        anchors.rightMargin: Metrics.scaled(10)
        visible: !root.section
        spacing: Metrics.scaled(root.compact ? 8 : 10)

        MaterialIcon {
            visible: root.iconName.length > 0
            Layout.preferredWidth: Metrics.scaled(root.compact ? 22 : 28)
            Layout.preferredHeight: width
            name: root.iconName
            iconSize: Metrics.scaled(root.compact ? 18 : 22)
            iconColor: root.checked || root.highlighted ? Theme.accent : root.actionable ? Theme.textSecondary :
                                                                                           Theme.textMuted

        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Metrics.scaled(1)

            AppText {
                Layout.fillWidth: true
                text: root.label
                color: root.actionable || root.highlighted ? Theme.textPrimary : Theme.textSecondary
                font.pixelSize: root.compact ? Metrics.metaSizePx + 2 : Metrics.bodySizePx + Metrics.scaled(2)
                font.weight: root.highlighted ? Font.DemiBold : Font.Medium
                maximumLineCount: 1
                elide: Text.ElideRight
            }
            SecondaryText {
                Layout.fillWidth: true
                visible: root.detail.length > 0
                text: root.detail
                color: Theme.textMuted
                font.pixelSize: root.compact ? Metrics.metaSizePx - 1 : Metrics.metaSizePx + Metrics.scaled(1)
                maximumLineCount: 1
                elide: Text.ElideRight
            }
        }

        RowLayout {
            visible: root.stepperVisible
            spacing: 0

            Item {
                Layout.preferredWidth: Math.max(Metrics.touchTargetPx, Metrics.scaled(root.compact ? 34 : 44))
                Layout.minimumWidth: Metrics.touchTargetPx
                Layout.preferredHeight: root.height

                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width - Metrics.scaled(4)
                    height: Metrics.scaled(root.compact ? 26 : 34)
                    radius: Theme.radiusSmall
                    color: minusHover.hovered && root.stepperEnabled ? Theme.bgHover : Theme.bgPanel
                    border.width: Theme.hoverBorderWidth
                    border.color: root.stepperEnabled ? Theme.borderStrong : Theme.border
                }

                AppText {
                    anchors.centerIn: parent
                    text: "−"
                    color: root.stepperEnabled ? Theme.textPrimary : Theme.textDisabled
                    font.pixelSize: root.compact ? Metrics.bodySizePx : Metrics.titleSizePx
                    font.weight: Font.DemiBold
                }
                TapHandler {
                    enabled: root.stepperEnabled
                    gesturePolicy: TapHandler.DragThreshold
                    onTapped: root.decreaseRequested()
                }
                HoverHandler {
                    id: minusHover
                    enabled: root.stepperEnabled
                }
            }

            Rectangle {
                Layout.preferredWidth: Math.max(root.stepperEditable ? Metrics.touchTargetPx : 0, Metrics.scaled(
                                                    root.compact ? 54 : 68))
                Layout.minimumWidth: root.stepperEditable ? Metrics.touchTargetPx : 0
                Layout.preferredHeight: root.height
                radius: Theme.radiusSmall
                color: speedInput.activeFocus ? Theme.bgPanel : "transparent"
                border.width: root.stepperInvalid ? Theme.focusBorderWidth : 0
                border.color: Theme.errorText

                SecondaryText {
                    anchors.fill: parent
                    visible: !root.stepperEditable
                    text: root.stepperText
                    color: root.stepperEnabled ? Theme.textPrimary : Theme.textSecondary
                    font.pixelSize: root.compact ? Metrics.metaSizePx + 2 : Metrics.bodySizePx + Metrics.scaled(2)
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                TextInput {
                    id: speedInput
                    anchors.fill: parent
                    visible: root.stepperEditable
                    enabled: root.stepperEnabled
                    text: root.stepperEditText
                    color: root.stepperInvalid ? Theme.errorText : root.stepperEnabled ? Theme.textPrimary :
                                                                                         Theme.textSecondary

                    selectionColor: Theme.accent
                    selectedTextColor: Theme.textPrimary
                    font.family: Typography.sans
                    font.hintingPreference: Typography.sansHinting
                    font.pixelSize: root.compact ? Metrics.metaSizePx + 2 : Metrics.bodySizePx + Metrics.scaled(2)
                    font.weight: Font.Normal
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    selectByMouse: true
                    validator: RegularExpressionValidator {
                        regularExpression: /[0-9.]*/
                    }
                    onActiveFocusChanged: {
                        if (activeFocus)
                            selectAll()
                        else if (!root.stepperInvalid)
                            text = root.stepperEditText
                    }
                    Keys.onReturnPressed: event => {
                        root.stepperAccepted(text)
                        event.accepted = true
                        if (!root.stepperInvalid) {
                            text = root.stepperEditText
                            focus = false
                        }
                    }
                    Keys.onEnterPressed: event => {
                        root.stepperAccepted(text)
                        event.accepted = true
                        if (!root.stepperInvalid) {
                            text = root.stepperEditText
                            focus = false
                        }
                    }
                }
            }

            Item {
                Layout.preferredWidth: Math.max(Metrics.touchTargetPx, Metrics.scaled(root.compact ? 34 : 44))
                Layout.minimumWidth: Metrics.touchTargetPx
                Layout.preferredHeight: root.height

                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width - Metrics.scaled(4)
                    height: Metrics.scaled(root.compact ? 26 : 34)
                    radius: Theme.radiusSmall
                    color: plusHover.hovered && root.stepperEnabled ? Theme.bgHover : Theme.bgPanel
                    border.width: Theme.hoverBorderWidth
                    border.color: root.stepperEnabled ? Theme.borderStrong : Theme.border
                }

                AppText {
                    anchors.centerIn: parent
                    text: "+"
                    color: root.stepperEnabled ? Theme.textPrimary : Theme.textDisabled
                    font.pixelSize: root.compact ? Metrics.bodySizePx : Metrics.titleSizePx
                    font.weight: Font.DemiBold
                }
                TapHandler {
                    enabled: root.stepperEnabled
                    gesturePolicy: TapHandler.DragThreshold
                    onTapped: root.increaseRequested()
                }
                HoverHandler {
                    id: plusHover
                    enabled: root.stepperEnabled
                }
            }
        }

        MaterialIcon {
            visible: root.checked && root.checkIconName.length > 0
            Layout.preferredWidth: Metrics.scaled(24)
            Layout.preferredHeight: width
            name: root.checkIconName
            iconSize: Metrics.scaled(21)
            iconColor: Theme.accent
        }
    }

    HoverHandler {
        id: hover
        enabled: root.actionable && !root.section
        onHoveredChanged: if (hovered)
                              root.hovered()
    }
    TapHandler {
        enabled: root.pointerActivationEnabled && root.actionable && !root.section && !root.stepperVisible
        gesturePolicy: TapHandler.DragThreshold
        onTapped: root.activated()
    }
}
