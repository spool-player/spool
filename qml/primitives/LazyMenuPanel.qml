pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"

Loader {
    id: root

    property bool open: false
    property var model: []
    // The owner's row, normally bound to the owner's own property. The panel
    // never writes it, so that binding survives; the person's moves arrive
    // through moved() instead.
    property int currentIndex: 0
    property Item edgeEscapeItem: null
    property string title: ""
    property string selectionStyle: "radio"
    property real maximumHeight: 430
    property var checkedFor: function (entry) {
        return Boolean(entry && entry.checked)
    }
    property bool resetVisible: false
    property bool resetEnabled: false
    readonly property var menuList: item ? item.menuList : null
    property bool rebuilding: false

    signal moved(int index)
    signal accepted(var entry, int index)
    signal dismissed
    signal reset

    active: open
    height: active ? maximumHeight : 0

    // A new model resets a ListView to its first row. Owners rebuild their
    // entries on every toggle and expand, so swap the model without telling
    // them and put the list back on their row.
    function syncModel() {
        if (!menuList)
            return
        rebuilding = true
        menuList.model = model
        rebuilding = false
        syncCurrent()
    }

    function syncCurrent() {
        if (menuList && menuList.currentIndex !== currentIndex)
            menuList.currentIndex = Math.min(currentIndex, menuList.count - 1)
    }

    onLoaded: syncModel()
    onModelChanged: syncModel()
    onCurrentIndexChanged: syncCurrent()

    sourceComponent: PopupMenuPanel {
        id: panel
        property alias menuList: list

        width: root.width
        open: true
        openHeight: Math.min(root.maximumHeight, list.contentHeight + (header.visible ? header.height + 8 : 0) + 20)

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 8

            RowLayout {
                id: header
                Layout.fillWidth: true
                visible: root.title.length > 0
                AppText {
                    Layout.fillWidth: true
                    text: root.title
                    font.pixelSize: Metrics.bodySizePx
                    font.weight: Font.DemiBold
                }
                ActionButton {
                    visible: root.resetVisible
                    text: "Reset"
                    enabled: root.resetEnabled
                    onClicked: root.reset()
                }
            }

            MenuListView {
                id: list
                Layout.fillWidth: true
                Layout.fillHeight: true
                entryProvider: index => root.model[index]
                edgeEscapeItem: root.edgeEscapeItem
                onCurrentIndexChanged: if (!root.rebuilding)
                                           root.moved(currentIndex)
                onDismissed: root.dismissed()
                onAccepted: index => root.accepted(root.model[index], index)

                delegate: MenuRow {
                    required property int index
                    required property var modelData
                    readonly property bool rowChecked: root.checkedFor(modelData)
                    width: list.width
                    section: Boolean(modelData && modelData.section)
                    actionable: !(modelData && (modelData.enabled === false || modelData.actionable === false))
                    label: modelData && modelData.label ? modelData.label : ""
                    detail: modelData && modelData.detail ? modelData.detail : ""
                    checked: rowChecked
                    iconName: modelData && modelData.iconName ? modelData.iconName : rowChecked ? (root.selectionStyle
                                                                                                   === "radio"
                                                                                                   ? "radio_button_checked" :
                                                                                                     "check_box") : (
                                                                                                      root.selectionStyle
                                                                                                      === "radio"
                                                                                                      ? "radio_button_unchecked" :
                                                                                                        "check_box_outline_blank")
                    highlighted: ListView.isCurrentItem && list.activeFocus
                    rowHeight: modelData && modelData.detail ? 54 : 48
                    checkIconName: ""
                    onHovered: list.currentIndex = index
                    onActivated: root.accepted(modelData, index)
                }
            }
        }
    }
}
