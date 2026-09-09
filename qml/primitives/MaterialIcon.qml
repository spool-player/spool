import QtQuick
import "../theme"

Text {
    id: root

    property string name: ""
    property color iconColor: "#EEEEEE"
    property int iconSize: 32

    text: name
    color: iconColor
    font: Qt.font({
                      "family": Typography.material,
                      "pixelSize": Math.max(1, root.iconSize),
                      "hintingPreference": Font.PreferNoHinting
                  })
    renderType: Text.QtRendering
    horizontalAlignment: Text.AlignHCenter
    verticalAlignment: Text.AlignVCenter
}
