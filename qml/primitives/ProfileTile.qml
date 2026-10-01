import QtQuick
import "../theme"

// An account: initial-avatar tile, name, and the server it belongs to.
FocusScope {
    id: root

    property int tileSize: Metrics.scaled(152)
    property string username: ""
    property string serverName: ""
    property bool needsSignIn: false
    property bool addTile: false
    property bool focused: activeFocus

    readonly property int labelHeight: Metrics.scaled(addTile || serverName.length === 0 ? 34 : 52)

    readonly property string initial: {
        const name = String(username).trim()
        return name.length > 0 ? name.charAt(0).toUpperCase() : "?"
    }
    // Stable decorative tints distinguish accounts, not network or connection
    // state. Keep them separate from the green used for online feedback.
    readonly property color avatarColor: {
        const palette = ["#303B4A", "#35394B", "#2D3D55", "#3E3549", "#343D43", "#393849"]
        const name = String(username)
        let hash = 0
        for (let i = 0; i < name.length; ++i)
            hash = ((hash << 5) - hash + name.charCodeAt(i)) | 0
        return palette[Math.abs(hash) % palette.length]
    }

    signal accepted
    signal contextRequested

    width: tileSize
    height: tileSize + labelHeight
    focus: true
    focusPolicy: Qt.StrongFocus

    Rectangle {
        id: avatar
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        width: root.tileSize
        height: root.tileSize
        radius: Theme.radiusLarge
        color: root.addTile ? Theme.bgRaised : root.avatarColor
        border.width: root.focused ? Theme.focusBorderWidth : (root.addTile || (hover.hovered
                                                                                && Metrics.pointerActive))
                                     ? Theme.hoverBorderWidth : 0
        border.color: root.focused ? Theme.accent : Theme.border
        antialiasing: true

        MaterialIcon {
            anchors.centerIn: parent
            visible: root.addTile
            name: "add"
            iconSize: Math.round(root.tileSize * 0.3)
            iconColor: root.focused ? Theme.accent : Theme.textSecondary
        }

        AppText {
            anchors.centerIn: parent
            visible: !root.addTile
            text: root.initial
            font.pixelSize: Math.round(root.tileSize * 0.4)
            font.weight: Font.DemiBold
        }

        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Metrics.scaled(8)
            width: Metrics.scaled(28)
            height: width
            radius: width / 2
            visible: root.needsSignIn
            color: Theme.errorPanel
            border.width: Theme.hoverBorderWidth
            border.color: Theme.errorText

            MaterialIcon {
                anchors.centerIn: parent
                name: "lock"
                iconSize: Metrics.scaled(16)
                iconColor: Theme.errorText
            }
        }
    }

    // Selection reads as a ring standing off the tile rather than a heavier
    // border on it: across a room the gap is what carries, and the artwork
    // underneath keeps its own edge.
    Rectangle {
        anchors.fill: avatar
        anchors.margins: -Metrics.scaled(5)
        radius: avatar.radius + Metrics.scaled(5)
        color: "transparent"
        border.width: root.focused ? Theme.focusBorderWidth : 0
        border.color: Theme.accent
        opacity: root.focused ? 0.6 : 0
        antialiasing: true

        Behavior on opacity {
            enabled: !Theme.reducedMotion
            NumberAnimation {
                duration: 110
            }
        }
    }

    Column {
        anchors.top: avatar.bottom
        anchors.topMargin: Metrics.scaled(12)
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: Metrics.scaled(2)

        AppText {
            width: parent.width
            text: root.username
            color: root.focused ? Theme.textPrimary : Theme.textSecondary
            font.pixelSize: Metrics.scaled(16)
            font.weight: Font.DemiBold
            horizontalAlignment: Text.AlignHCenter
            maximumLineCount: 1
            elide: Text.ElideRight
        }

        SecondaryText {
            width: parent.width
            visible: !root.addTile && root.serverName.length > 0
            text: root.serverName
            color: Theme.textMuted
            font.pixelSize: Metrics.scaled(13)
            horizontalAlignment: Text.AlignHCenter
            maximumLineCount: 1
            elide: Text.ElideRight
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: mouse => {
            if (mouse.button === Qt.RightButton)
                root.contextRequested()
            else
                root.accepted()
        }
        onPressAndHold: root.contextRequested()
    }

    HoverHandler {
        id: hover
    }
}
