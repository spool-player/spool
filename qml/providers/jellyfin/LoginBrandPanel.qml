import QtQuick
import QtQuick.Layouts
import "../../theme"
import "../../primitives"

// The header of the sign-in screen: where you are, and the way back out.
// Nothing else in here takes focus, so a remote never walks through it on the
// way to the form.
Item {
    id: root

    // Room is short — a landscape phone, or an on-screen keyboard eating half
    // the panel — so the version and some of the weight give up their space.
    property bool dense: false
    property bool backVisible: false

    readonly property alias backControl: backButton

    signal backRequested

    implicitHeight: headerRow.implicitHeight

    // Back sits at the head of the identity rather than floating over the
    // page, so the form below never has to leave a hole for it. The slot keeps
    // its width whether or not there is anywhere to go back to.
    RowLayout {
        id: headerRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: Metrics.scaled(12)

        Item {
            Layout.preferredWidth: backButton.width
            Layout.preferredHeight: backButton.height

            IconButton {
                id: backButton
                anchors.centerIn: parent
                iconName: "arrow_back"
                visible: root.backVisible
                enabled: visible
                onClicked: root.backRequested()
            }
        }

        Image {
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: Metrics.scaled(root.dense ? 24 : 30)
            Layout.preferredHeight: Layout.preferredWidth
            source: "qrc:/icons/spool.svg"
            sourceSize.width: Layout.preferredWidth
            sourceSize.height: Layout.preferredHeight
            fillMode: Image.PreserveAspectFit
            smooth: true
            asynchronous: true
        }

        AppText {
            Layout.alignment: Qt.AlignVCenter
            text: "Spool"
            font.pixelSize: Metrics.scaled(root.dense ? 19 : 23)
            font.weight: Font.DemiBold
        }

        AppText {
            Layout.alignment: Qt.AlignBaseline
            visible: !root.dense
            text: Qt.application.version
            color: Theme.textMuted
            font.pixelSize: Metrics.metaSizePx
            font.weight: Font.Medium
        }

        Item {
            Layout.fillWidth: true
        }
    }
}
