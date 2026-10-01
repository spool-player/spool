import QtQuick
import "../theme"

// A provider's icon from its package, or its initial on a tint derived from
// its id while the image loads or when there is none.
Rectangle {
    id: root

    property url source
    property string name: ""
    property string seed: name
    // A Material icon for providers without an image of their own.
    property string symbol: ""

    radius: Math.round(width * 0.22)
    color: image.status === Image.Ready ? Theme.bgPanel : symbol ? Theme.accentDim : Theme.libraryTint(seed)
    antialiasing: true

    AppText {
        anchors.centerIn: parent
        visible: image.status !== Image.Ready && !root.symbol
        text: root.name.length > 0 ? root.name.charAt(0).toUpperCase() : "?"
        color: "white"
        font.pixelSize: Math.round(root.height * 0.46)
        font.weight: Font.DemiBold
    }

    MaterialIcon {
        anchors.centerIn: parent
        visible: image.status !== Image.Ready && !!root.symbol
        name: root.symbol
        iconColor: "white"
        iconSize: Math.round(root.height * 0.56)
    }

    Image {
        id: image
        anchors.fill: parent
        anchors.margins: Math.round(root.width * 0.14)
        source: root.source
        sourceSize.width: width * 2
        sourceSize.height: height * 2
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        smooth: true
    }
}
