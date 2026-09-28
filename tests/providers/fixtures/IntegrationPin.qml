import QtQuick
import QtQuick.Controls

FocusScope {
    required property var provider
    Column {
        anchors.centerIn: parent
        TextField {
            id: pin
            objectName: "integrationPin"
            echoMode: TextInput.Password
            placeholderText: "PIN"
        }
        Button {
            objectName: "integrationSubmit"
            text: "Unlock"
            onClicked: provider.complete({
                                             pin: pin.text
                                         })
        }
    }
}
