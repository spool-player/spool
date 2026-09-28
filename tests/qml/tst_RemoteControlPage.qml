import QtQuick
import QtTest
import Spool
import "../../qml/pages" as Pages

TestCase {
    id: testCase
    name: "RemoteControlPage"
    width: 1280
    height: 720
    visible: true
    when: windowShown
    readonly property var remoteController: remote
    readonly property var appController: app

    QtObject {
        id: remote
        property var targets: []
        property string selectedTargetId: ""
        property var selectedTarget: ({})
        property var state: ({})
        property string positionTicks: "100000000"
        property string runtimeTicks: "1000000000"
        property var queue: []
        property bool queueHasMore: false
        property bool queueBusy: false
        property bool busy: false
        property string problem: ""
        property bool chooserVisible: false
        property bool queueVisible: false
        property var sent: []
        signal selectionChanged
        function setChooserVisible(value) {
            chooserVisible = value
        }
        function setQueueVisible(value) {
            queueVisible = value
        }
        function selectTarget(id) {
            selectedTargetId = id
            selectedTarget = targets.find(target => target.id === id) || ({})
            selectionChanged()
        }
        function disconnectTarget() {
            selectTarget("")
        }
        function send(command) {
            sent = sent.concat([command])
        }
        function refreshTargets() {
        }
        function requestQueuePage(reset) {
        }
        function openAdvancedControls() {
        }
    }
    QtObject {
        id: app
        property int transfers: 0
        function transferPlaybackToRemote() {
            ++transfers
        }
    }
    QtObject {
        id: player
        property bool sessionActive: true
    }
    Component {
        id: pageComponent
        Pages.RemoteControlPage {
            width: testCase.width
            height: testCase.height
            remote: testCase.remoteController
            app: testCase.appController
            localPlayer: player
        }
    }
    SignalSpy {
        id: rowsSpy
        signalName: "rowsChanged"
    }

    function test_compactChooser() {
        const page = createTemporaryObject(pageComponent, testCase, {
                                               compact: true,
                                               width: 560,
                                               height: 600
                                           })
        verify(page)
        compare(page.rows[0].key, "target:")
        verify(!page.rows.some(row => row.key === "chooser"))
        verify(remote.chooserVisible)
        page.routeKey(Qt.Key_Down, "press", false)
        verify(page.focusedIndex > 0)
    }

    function init() {
        remote.selectedTargetId = ""
        remote.selectedTarget = ({})
        remote.targets = [
                    {
                        id: "account/target",
                        name: "Living room",
                        queueEditing: "replace"
                    }
                ]
        remote.state = {
            state: "playing",
            title: "Film",
            commands: ["play", "pause", "seek", "volume", "subtitleTrack", "queuePlay", "queueRemove", "queueMove"],
            subtitleTracks: [
                {
                    id: "7",
                    label: "English",
                    selected: true
                }
            ]
        }
        remote.positionTicks = "100000000"
        remote.runtimeTicks = "1000000000"
        remote.queue = [
                    {
                        itemId: "account/film",
                        entryId: "first",
                        title: "Film"
                    },
                    {
                        itemId: "account/film",
                        entryId: "second",
                        title: "Film"
                    }
                ]
        remote.sent = []
        remote.busy = false
        remote.queueBusy = false
        app.transfers = 0
    }
    function page() {
        const item = createTemporaryObject(pageComponent, testCase)
        verify(item !== null)
        return item
    }
    function activate(item, key) {
        item.focusedKey = key
        verify(item.focusedIndex >= 0, "Missing row: " + key)
        item.activate()
    }
    function test_selectionNeverTransfers() {
        const item = page()
        compare(item.rows[1].targetId, "")
        activate(item, "target:account/target")
        compare(remote.selectedTargetId, "account/target")
        compare(app.transfers, 0)
        compare(remote.sent.length, 0)
        activate(item, "transfer")
        compare(app.transfers, 1)
    }
    function test_seekEditingAndPollKeepsRowsStable() {
        remote.selectTarget("account/target")
        const item = page()
        rowsSpy.target = item
        rowsSpy.clear()
        activate(item, "seek")
        item.routeKey(Qt.Key_Right, "press", false)
        compare(item.editedValue, 20)
        remote.positionTicks = "120000000"
        compare(rowsSpy.count, 0)
        compare(item.editedValue, 20)
        item.activate()
        compare(remote.sent[0].positionTicks, "200000000")
        activate(item, "seek")
        item.routeKey(Qt.Key_Right, "press", false)
        verify(item.back())
        compare(remote.sent.length, 1)
        rowsSpy.target = null
    }
    function test_duplicateOccurrenceAndSubtitleOff() {
        remote.selectTarget("account/target")
        const item = page()
        activate(item, "queue")
        verify(remote.queueVisible)
        activate(item, "entry:second:remove")
        compare(remote.sent[0].entryId, "second")
        activate(item, "entry:second:up")
        compare(remote.sent[1].index, 0)
        compare(remote.sent[1].afterEntryId, null)
        activate(item, "subtitle:off")
        compare(remote.sent[2].action, "subtitleTrack")
        compare(remote.sent[2].trackId, null)
        item.visible = false
        verify(!remote.queueVisible)
        verify(!remote.chooserVisible)
    }
    function test_unknownValuesAndLostCapabilities() {
        remote.selectTarget("account/target")
        const item = page()
        verify(!item.rows.some(row => row.key === "volume"))
        item.focusedKey = "seek"
        remote.state = {
            state: "playing",
            commands: ["pause"]
        }
        verify(!item.rows.some(row => row.key === "seek"))
        verify(item.focusedIndex >= 0)
        remote.busy = true
        activate(item, "disconnect")
        compare(remote.selectedTargetId, "")
        compare(remote.sent.length, 0)
    }
    function test_queueMutationKeepsFocusedRowInView() {
        Metrics.keyboardFocusActive = true
        remote.selectTarget("account/target")
        const entries = []
        for (let index = 0; index < 25; ++index)
            entries.push({
                             itemId: "account/film",
                             entryId: "entry" + index,
                             title: "Occurrence " + index
                         })
        remote.queue = entries
        const item = page()
        item.queueOpen = true
        item.focusedKey = "entry:entry20:remove"
        item.revealFocusedRow()
        const view = findChild(item, "remoteControlsList")
        verify(view !== null)
        tryVerify(function () {
            return view.currentItem && view.contentY > 0
        })
        remote.busy = true
        remote.queue = entries.filter(entry => entry.entryId !== "entry20")
        remote.busy = false
        tryVerify(function () {
            return item.focusedIndex >= 0 && view.currentItem && view.currentItem.y >= view.contentY - 1
                    && view.currentItem.y + view.currentItem.height <= view.contentY + view.height + 1
        })
    }
}
