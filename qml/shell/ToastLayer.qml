import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"

Item {
    id: root
    property string message: ""
    property var pending: []
    property string actionText: ""
    property var actionCallback: null

    function clearAction() {
        actionText = ""
        actionCallback = null
    }

    // Some messages are acknowledgements rather than news — connecting to a
    // device the user just picked, say — and want to be gone before they are
    // read twice.
    readonly property int defaultDurationMs: 2400
    readonly property int briefDurationMs: 1100

    function show(text, durationMs) {
        const normalized = String(text || "")
        if (normalized.length === 0)
            return
        const duration = durationMs > 0 ? durationMs : defaultDurationMs
        if (message.length === 0) {
            message = normalized
            clearAction()
            timer.interval = duration
            timer.restart()
            return
        }
        const next = pending.slice()
        next.push({
                      "text": normalized,
                      "duration": duration
                  })
        while (next.length > 2)
            next.shift()
        pending = next
    }

    function showAction(text, label, callback) {
        const normalized = String(text || "")
        if (normalized.length === 0)
            return
        message = normalized
        actionText = String(label || "")
        actionCallback = callback
        pending = []
        timer.interval = defaultDurationMs
        timer.restart()
    }

    function triggerAction() {
        const callback = actionCallback
        message = ""
        clearAction()
        timer.stop()
        if (callback)
            callback()
        showNext()
    }

    function showNext() {
        clearAction()
        if (pending.length === 0) {
            message = ""
            return
        }
        const next = pending.slice()
        const entry = next.shift()
        message = entry.text
        pending = next
        timer.interval = entry.duration > 0 ? entry.duration : defaultDurationMs
        timer.restart()
    }

    Timer {
        id: timer
        interval: root.defaultDurationMs
        onTriggered: root.showNext()
    }

    Surface {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Metrics.scaled(34)
        width: Math.min(parent.width - Metrics.pageMarginPx * 2, toastContent.implicitWidth + Metrics.scaled(42))
        height: Math.max(Metrics.controlHeightPx, toastContent.implicitHeight + Metrics.scaled(24))
        visible: root.message.length > 0
        elevated: true

        RowLayout {
            id: toastContent
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: Metrics.scaled(21)
            anchors.rightMargin: Metrics.scaled(21)
            spacing: Metrics.scaled(16)

            AppText {
                Layout.fillWidth: true
                text: root.message
                color: Theme.textPrimary
                font.pixelSize: Metrics.bodySizePx
                wrapMode: Text.Wrap
            }

            ActionButton {
                visible: root.actionText.length > 0
                Layout.alignment: Qt.AlignVCenter
                text: root.actionText
                kind: "flat"
                onClicked: root.triggerAction()
            }
        }
    }
}
