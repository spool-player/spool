pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"
import "../providers/jellyfin"

// The provider selection page: choose a media backend or download the
// portable Jellyfin provider before proceeding to authentication or browsing.
FocusScope {
    id: root

    property var shell

    readonly property bool keyboardVisible: Qt.inputMethod.visible
    readonly property bool dense: keyboardVisible || Metrics.units(height) < 620
    readonly property var providers: Sources.availableProviders
    readonly property int providerCount: providers.length
    readonly property bool canGoBack: Router.canGoBack || (shell && shell.canCancelSwitchUser)

    readonly property int tileSize: {
        const preferred = Metrics.scaled(Metrics.laneAtLeast(width, "wide") ? 176 : Metrics.laneAtLeast(width,
                                                                                                        "regular")
                                                                              ? 156 : 132)
        return Math.max(Metrics.scaled(80), Math.min(preferred, width - cellPadding * 2))
    }
    readonly property int cellPadding: Metrics.scaled(16)

    focus: true

    function focusDefault() {
        if (grid.currentIndex < 0 && grid.count > 0)
            grid.currentIndex = 0
        InputKeys.focus(grid)
    }

    Component.onCompleted: {
        focusDefault()
    }

    Connections {
        target: Sources
        function onProviderInstalled(moduleId) {
            if (moduleId === "spool.jellyfin") {
                Sources.switchProvider("jellyfin")
                Router.reset("login")
            }
        }
    }

    Item {
        id: brandHeader
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: Metrics.pageMarginPx
        anchors.rightMargin: Metrics.pageMarginPx
        anchors.topMargin: Metrics.pageMarginPx
        height: brandPanel.implicitHeight

        LoginBrandPanel {
            id: brandPanel
            anchors.fill: parent
            dense: root.dense
            backVisible: root.canGoBack
            onBackRequested: {
                if (Router.canGoBack)
                    Router.back()
                else
                    Router.reset("login")
            }
        }
    }

    Item {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: brandHeader.bottom
        anchors.bottom: parent.bottom
        anchors.margins: Metrics.pageMarginPx

        ColumnLayout {
            id: centerColumn
            anchors.centerIn: parent
            width: Math.min(parent.width, Math.max(Metrics.scaled(320), grid.width))
            spacing: Metrics.scaled(18)

            ColumnLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: Metrics.scaled(6)

                AppText {
                    Layout.alignment: Qt.AlignHCenter
                    text: "Choose a provider"
                    font.pixelSize: Metrics.titleSizePx
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                }

                AppText {
                    Layout.alignment: Qt.AlignHCenter
                    text: "Select a media source to connect to Spool"
                    color: Theme.textSecondary
                    font.pixelSize: Metrics.bodySizePx
                    horizontalAlignment: Text.AlignHCenter
                }
            }

            GridView {
                id: grid
                Layout.alignment: Qt.AlignHCenter

                readonly property int cellSpan: root.tileSize + root.cellPadding * 2
                readonly property int columnsInUse: Math.max(1, Math.min(root.providerCount, Math.floor((content.width
                                                                                                         - Metrics.scaled(
                                                                                                             32)) / cellSpan)))

                width: cellSpan * columnsInUse
                height: root.tileSize + Metrics.scaled(root.dense ? 70 : 100) + root.cellPadding
                cellWidth: cellSpan
                cellHeight: height
                clip: false
                focus: true
                keyNavigationEnabled: false
                boundsBehavior: Flickable.StopAtBounds
                model: root.providers
                currentIndex: 0

                function activateCurrent() {
                    if (currentItem)
                        currentItem.activate()
                }

                Keys.onLeftPressed: {
                    if (currentIndex > 0)
                        currentIndex--
                }
                Keys.onRightPressed: {
                    if (currentIndex < count - 1)
                        currentIndex++
                }
                Keys.onSelectPressed: activateCurrent()
                Keys.onEnterPressed: activateCurrent()
                Keys.onReturnPressed: activateCurrent()

                FastWheelHandler {
                    flickable: grid
                }

                delegate: Item {
                    id: cell
                    required property var modelData
                    required property int index

                    width: grid.cellWidth
                    height: grid.cellHeight

                    function activate() {
                        tileScope.accepted()
                    }

                    FocusScope {
                        id: tileScope
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.top
                        anchors.topMargin: root.cellPadding
                        width: root.tileSize
                        height: root.tileSize + Metrics.scaled(80)
                        focus: cell.GridView.isCurrentItem && grid.activeFocus

                        readonly property bool isInstalled: Boolean(cell.modelData.installed)
                        readonly property bool isJellyfin: cell.modelData.id === "spool.jellyfin"
                        readonly property bool isDownloading: isJellyfin && Sources.isInstalling

                        signal accepted

                        onAccepted: {
                            grid.currentIndex = cell.index
                            InputKeys.focus(grid)
                            if (isJellyfin) {
                                if (isInstalled) {
                                    Sources.switchProvider("jellyfin")
                                    Router.reset("login")
                                } else if (!isDownloading) {
                                    Sources.installProvider("spool.jellyfin")
                                }
                            } else if (cell.modelData.id === "local") {
                                Sources.switchProvider("local")
                                Router.reset("home")
                            }
                        }

                        Rectangle {
                            id: tileAvatar
                            anchors.top: parent.top
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: root.tileSize
                            height: root.tileSize
                            radius: Theme.radiusLarge
                            color: tileScope.focus ? Theme.bgRaised : Theme.bg
                            border.width: tileScope.focus ? Theme.focusBorderWidth : hoverArea.containsMouse
                                                            ? Theme.hoverBorderWidth : 1
                            border.color: tileScope.focus ? Theme.accent : hoverArea.containsMouse ? Theme.borderRaised :
                                                                                                     Theme.border
                            antialiasing: true

                            MaterialIcon {
                                anchors.centerIn: parent
                                name: cell.modelData.id === "local" ? "video_library" : "dns"
                                iconSize: Math.round(root.tileSize * 0.42)
                                iconColor: tileScope.focus ? Theme.accent : Theme.textPrimary
                                visible: !tileScope.isDownloading
                            }

                            // Download progress spinner/indicator
                            ColumnLayout {
                                anchors.centerIn: parent
                                visible: tileScope.isDownloading
                                spacing: Metrics.scaled(8)

                                MaterialIcon {
                                    Layout.alignment: Qt.AlignHCenter
                                    name: "cloud_download"
                                    iconSize: Math.round(root.tileSize * 0.36)
                                    iconColor: Theme.accent
                                }

                                AppText {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: Sources.installProgress + "%"
                                    font.pixelSize: Metrics.metaSizePx
                                    font.weight: Font.Bold
                                    color: Theme.accent
                                }
                            }
                        }

                        // Focus ring offset
                        Rectangle {
                            anchors.fill: tileAvatar
                            anchors.margins: -Metrics.scaled(5)
                            radius: tileAvatar.radius + Metrics.scaled(5)
                            color: "transparent"
                            border.width: Theme.focusRingWidth
                            border.color: Theme.accent
                            visible: tileScope.focus
                            antialiasing: true
                        }

                        ColumnLayout {
                            anchors.top: tileAvatar.bottom
                            anchors.topMargin: Metrics.scaled(10)
                            anchors.left: parent.left
                            anchors.right: parent.right
                            spacing: Metrics.scaled(3)

                            AppText {
                                Layout.fillWidth: true
                                text: String(cell.modelData.name || "")
                                font.pixelSize: Metrics.bodySizePx
                                font.weight: Font.DemiBold
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                            }

                            AppText {
                                Layout.fillWidth: true
                                text: tileScope.isDownloading ? Sources.installStatus : tileScope.isInstalled ? "Ready" :
                                                                                                                "Download & Install"
                                font.pixelSize: Metrics.metaSizePx
                                color: tileScope.isDownloading ? Theme.accent : tileScope.isInstalled
                                                                 ? Theme.textSecondary : Theme.accent
                                font.weight: tileScope.isInstalled ? Font.Normal : Font.Medium
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                            }
                        }

                        MouseArea {
                            id: hoverArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                tileScope.accepted()
                            }
                        }
                    }
                }
            }

            // Error display if installation failed
            AppText {
                Layout.alignment: Qt.AlignHCenter
                visible: Sources.installError.length > 0
                text: Sources.installError
                color: Theme.errorText
                font.pixelSize: Metrics.metaSizePx
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }
}
