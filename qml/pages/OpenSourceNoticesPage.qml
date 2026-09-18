pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

FocusScope {
    id: root

    property var shell
    property bool contentReady: true
    readonly property int contentMargin: Metrics.pageMarginPx
    readonly property var components: [
        {
            "name": "This application",
            "license": "MPL-2.0",
            "source": "github.com/spool-player/spool"
        },
        {
            "name": "mpv",
            "license": "GPL-2.0-or-later / LGPL-2.1-or-later",
            "source": "github.com/spool-player/mpv"
        },
        {
            "name": "FFmpeg",
            "license": "LGPL-2.1-or-later",
            "source": "ffmpeg.org"
        },
        {
            "name": "Qt 6",
            "license": "LGPL-3.0 / GPL",
            "source": "qt.io"
        },
        {
            "name": "QCoro",
            "license": "MIT",
            "source": "github.com/danvratil/qcoro"
        },
        {
            "name": "libass",
            "license": "ISC",
            "source": "github.com/libass/libass"
        },
        {
            "name": "libplacebo",
            "license": "LGPL-2.1-or-later",
            "source": "code.videolan.org/videolan/libplacebo"
        },
        {
            "name": "libdovi",
            "license": "MIT",
            "source": "github.com/quietvoid/dovi_tool"
        },
        {
            "name": "Atkinson Hyperlegible",
            "license": "SIL Open Font License 1.1",
            "source": "brailleinstitute.org/freefont"
        },
        {
            "name": "IBM Plex Sans",
            "license": "SIL Open Font License 1.1",
            "source": "github.com/IBM/plex"
        },
        {
            "name": "PT Root UI",
            "license": "SIL Open Font License 1.1",
            "source": "paratype.com/fonts/pt/pt-root-ui"
        },
        {
            "name": "Google Material Icons",
            "license": "Apache License 2.0",
            "source": "fonts.google.com/icons"
        }
    ]

    focus: true
    Component.onCompleted: InputKeys.focus(root)

    function scrollBy(delta) {
        const maximum = Math.max(0, noticesFlick.contentHeight - noticesFlick.height)
        noticesFlick.contentY = Math.max(0, Math.min(maximum, noticesFlick.contentY + delta))
    }

    function routeKey(key, phase, repeat) {
        if (phase === "release" && InputKeys.isDirection(key))
            return true
        if (key === Qt.Key_Up) {
            if (noticesFlick.contentY <= 0 && shell)
                shell.focusNavBar()
            else
                scrollBy(-Metrics.scaled(120))
            return true
        }
        if (key === Qt.Key_Down) {
            scrollBy(Metrics.scaled(120))
            return true
        }
        if (key === Qt.Key_PageUp) {
            scrollBy(-noticesFlick.height * 0.8)
            return true
        }
        if (key === Qt.Key_PageDown) {
            scrollBy(noticesFlick.height * 0.8)
            return true
        }
        return false
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    Flickable {
        id: noticesFlick
        anchors.fill: parent
        contentWidth: width
        contentHeight: Math.max(height, contentColumn.implicitHeight + root.contentMargin * 2)
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Behavior on contentY {
            enabled: !Theme.reducedMotion
            NumberAnimation {
                duration: 120
                easing.type: Easing.OutCubic
            }
        }

        FastWheelHandler {
            flickable: noticesFlick
        }

        ColumnLayout {
            id: contentColumn
            x: root.contentMargin
            y: root.contentMargin
            width: noticesFlick.width - root.contentMargin * 2
            spacing: Metrics.scaled(18)

            AppText {
                Layout.fillWidth: true
                text: "Open-source notices"
                font.pixelSize: Metrics.titleSizePx + Metrics.scaled(8)
                font.weight: Font.DemiBold
            }

            SectionHeader {
                Layout.fillWidth: true
                title: "Acknowledgements"
            }

            Surface {
                Layout.fillWidth: true
                Layout.preferredHeight: acknowledgementColumn.implicitHeight + Metrics.scaled(36)
                elevated: true
                baseColor: Theme.bgPanel

                ColumnLayout {
                    id: acknowledgementColumn
                    anchors.fill: parent
                    anchors.margins: Metrics.scaled(18)
                    spacing: Metrics.scaled(14)

                    AppText {
                        Layout.fillWidth: true
                        text: "Kodi developers"
                        font.pixelSize: Metrics.bodySizePx + Metrics.scaled(3)
                        font.weight: Font.DemiBold
                    }

                    AppText {
                        Layout.fillWidth: true
                        text: "Their Starfish implementation both showed me this was possible and made it much easier."
                        color: Theme.textSecondary
                        font.pixelSize: Metrics.bodySizePx
                        wrapMode: Text.Wrap
                    }

                    AppText {
                        Layout.fillWidth: true
                        text: "mpv developers"
                        font.pixelSize: Metrics.bodySizePx + Metrics.scaled(3)
                        font.weight: Font.DemiBold
                    }

                    AppText {
                        Layout.fillWidth: true
                        text: "Thank you for making a player so fast and extensible it seems almost divinely created."
                        color: Theme.textSecondary
                        font.pixelSize: Metrics.bodySizePx
                        wrapMode: Text.Wrap
                    }

                    AppText {
                        Layout.fillWidth: true
                        text: "Jellyfin developers"
                        font.pixelSize: Metrics.bodySizePx + Metrics.scaled(3)
                        font.weight: Font.DemiBold
                    }

                    AppText {
                        Layout.fillWidth: true
                        text: "Thank you for the server, APIs, documentation, and wider free-software media ecosystem."
                        color: Theme.textSecondary
                        font.pixelSize: Metrics.bodySizePx
                        wrapMode: Text.Wrap
                    }

                    AppText {
                        Layout.fillWidth: true
                        text: "Qt developers"
                        font.pixelSize: Metrics.bodySizePx + Metrics.scaled(3)
                        font.weight: Font.DemiBold
                    }

                    AppText {
                        Layout.fillWidth: true
                        text: "Thank you for the application and user-interface toolkit."
                        color: Theme.textSecondary
                        font.pixelSize: Metrics.bodySizePx
                        wrapMode: Text.Wrap
                    }
                }
            }

            SectionHeader {
                Layout.fillWidth: true
                title: "License and corresponding source"
            }

            AppText {
                Layout.fillWidth: true
                text: "This application is free software licensed under MPL-2.0. Corresponding source is available at github.com/spool-player/spool. The modified mpv source is available at github.com/spool-player/mpv."
                color: Theme.textSecondary
                font.pixelSize: Metrics.bodySizePx
                wrapMode: Text.Wrap
            }

            Repeater {
                model: root.components

                Surface {
                    id: componentCard

                    required property var modelData

                    Layout.fillWidth: true
                    Layout.preferredHeight: componentRow.implicitHeight + Metrics.scaled(30)
                    baseColor: Theme.bgRaised

                    RowLayout {
                        id: componentRow
                        anchors.fill: parent
                        anchors.margins: Metrics.scaled(15)
                        spacing: Metrics.scaled(20)

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Metrics.scaled(4)

                            AppText {
                                Layout.fillWidth: true
                                text: componentCard.modelData.name
                                font.pixelSize: Metrics.bodySizePx + Metrics.scaled(2)
                                font.weight: Font.Medium
                            }

                            SecondaryText {
                                Layout.fillWidth: true
                                text: componentCard.modelData.source
                                color: Theme.textMuted
                                font.pixelSize: Metrics.metaSizePx + Metrics.scaled(2)
                                elide: Text.ElideRight
                            }
                        }

                        MetadataChip {
                            text: componentCard.modelData.license
                        }
                    }
                }
            }

            AppText {
                Layout.fillWidth: true
                Layout.bottomMargin: root.contentMargin
                text: "The exact runtime set varies by platform and build configuration. Copyright remains with each project's authors and contributors. Project names are used only for attribution and do not imply endorsement."
                color: Theme.textMuted
                font.pixelSize: Metrics.metaSizePx + Metrics.scaled(2)
                wrapMode: Text.Wrap
            }
        }
    }
}
