import QtQuick

FocusScope {
    id: root
    required property var action
    property bool loaded: false
    property bool failed: false
    width: 640
    height: 480
    focus: true

    Component.onCompleted: action.requestList("candidates", {
                                                  count: 2000
                                              }).then(function () {
                                                  root.loaded = true
                                              }, function () {
                                                  root.failed = true
                                              })
    Component.onDestruction: action.close()
    Keys.onEscapePressed: action.close()

    ListView {
        anchors.fill: parent
        model: root.action.rows
        delegate: Text {
            required property var record
            width: ListView.view.width
            height: 40
            text: record.title
            textFormat: Text.PlainText
        }
    }

    function choose(variantId) {
        action.complete({
                            variantId: variantId,
                            sourceId: "forged-source"
                        })
    }
}
