import QtQuick
import "../theme"

Item {
    id: root
    property string imageUrl: ""
    property string fallbackText: ""
    // A library or an album with no artwork reads better as its own glyph on a
    // tinted field than as the word "music" in a grey box.
    property string fallbackIcon: ""
    property color fallbackTint: "transparent"
    property bool artworkVisible: true
    property bool artworkEnabled: true
    property bool bordered: true
    readonly property bool artworkReady: !artworkEnabled || imageUrl.length === 0 || artwork.status === Image.Ready || artwork.status
                                         === Image.Error

    function artworkSource(url) {
        if (url.indexOf("http://") === 0 || url.indexOf("https://") === 0 || url.indexOf("spool-thumbnail:") === 0)
            return "image://artwork/" + encodeURIComponent(url)
        return url
    }

    readonly property bool showingFallback: !artworkEnabled || imageUrl.length === 0 || artwork.status === Image.Error

    Rectangle {
        id: frame
        anchors.fill: parent
        radius: Theme.radiusMedium
        color: root.showingFallback && root.fallbackTint.a > 0 ? root.fallbackTint : Theme.bgRaised
        border.width: root.bordered ? Theme.hoverBorderWidth : 0
        border.color: root.showingFallback && root.fallbackTint.a > 0 ? "transparent" : Theme.border
        clip: true

        Loader {
            anchors.centerIn: parent
            active: root.showingFallback && root.fallbackIcon.length > 0
            sourceComponent: MaterialIcon {
                name: root.fallbackIcon
                iconColor: Qt.rgba(1, 1, 1, 0.82)
                iconSize: Math.round(Math.min(frame.width, frame.height) * 0.42)
            }
        }

        Loader {
            anchors.fill: parent
            active: root.showingFallback && root.fallbackIcon.length === 0
            sourceComponent: SecondaryText {
                anchors.centerIn: parent
                width: parent.width - Metrics.scaled(20)
                text: root.fallbackText.length > 0 ? root.fallbackText : "..."
                color: Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                font.pixelSize: Metrics.scaled(13)
            }
        }

        Image {
            id: artwork
            anchors.fill: parent
            anchors.margins: frame.border.width
            source: root.artworkEnabled && root.imageUrl.length > 0 ? root.artworkSource(root.imageUrl) : ""
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            // Never display the previous delegate's artwork after its source
            // changes. The fallback remains visible while the new image loads.
            retainWhileLoading: false
            cache: !Platform.isTV
            smooth: true
            mipmap: false
            // Artwork corners are square. Rounding them meant giving every
            // tile its own layer and a MultiEffect mask -- a framebuffer and a
            // second render pass each, twenty-nine of them during a switch to
            // home. That was affordable while the incubator spread a cold
            // build over eighteen frames and the render thread only used 6 of
            // its 16.7 ms; once the build happened in one go it became the
            // thing holding the switch up, at 21.9 ms a frame.
            //
            // Dropping it takes a cold switch to home from 245 ms to 165 and
            // the library scroll's median settle from 77 ms to 41. Against a
            // captured frame the difference is 0.68% of pixels, because the
            // frame behind still carries its own radius and border, which is
            // what actually reads as the card's shape.
            //
            // Baking the radius into the decode is the way to have both, but
            // it needs the provider to return the tile's exact aspect, or
            // PreserveAspectCrop crops the baked corners off.
            opacity: root.artworkVisible ? 1 : 0
            Behavior on opacity {
                NumberAnimation {
                    duration: Theme.reducedMotion ? 0 : 90
                }
            }
        }
    }
}
