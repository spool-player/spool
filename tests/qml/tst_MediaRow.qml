import QtQuick
import QtTest
import "../../qml/primitives" as Primitives

TestCase {
    id: testCase
    name: "MediaRow"
    width: 640
    height: 240
    visible: true
    when: windowShown
    property var openedContext: null
    property var moves: []
    property int headerActivations: 0
    property var initialModel: null

    QtObject {
        id: shellHost
        function finishItemMenuOpeningGesture() {
        }
    }

    Primitives.MediaRow {
        id: libraryRow
        width: parent.width
        cardKind: "library"
        cardWidth: 156
        cardGap: 16
        shell: shellHost
        contextMenu: (item, anchor, context) => {
            testCase.openedContext = {
                "item": item,
                "context": context
            }
            return true
        }
        moveItem: (from, to) => {
            testCase.moves = testCase.moves.concat([
                                                       {
                                                           "from": from,
                                                           "to": to
                                                       }
                                                   ])
            return true
        }
        model: [
            {
                "name": "Movies",
                "libraryId": "aaaaaaaa:movies",
                "collectionType": "movies",
                "item": ({})
            },
            {
                "name": "Shows",
                "libraryId": "bbbbbbbb:movies",
                "collectionType": "tvshows",
                "item": ({})
            }
        ]
    }

    function initTestCase() {
        initialModel = libraryRow.model
    }

    function init() {
        libraryRow.finishMove()
        libraryRow.model = initialModel
        libraryRow.reserveWhenEmpty = false
        libraryRow.keyboardFocusActive = true
        libraryRow.currentIndex = 0
        libraryRow.headerAction = null
        openedContext = null
        moves = []
        headerActivations = 0
    }

    function test_reorderRequiresExplicitMoveChoice() {
        verify(libraryRow.longPress())
        compare(openedContext.item.libraryId, "aaaaaaaa:movies")
        compare(libraryRow.moveMode, false, "a hold opens the menu, not move mode")
        compare(libraryRow.beginDrag(0, 180, 20, 10, 20), false, "normal pointer drags cannot reorder")
        compare(libraryRow.moveSelected(1), false, "remote reorder is gated too")
        compare(moves.length, 0)

        verify(openedContext.context.row.beginMoveById(openedContext.item.libraryId))
        compare(libraryRow.moveMode, true)
        verify(libraryRow.moveSelected(1))
        compare(moves, [
                    {
                        "from": 0,
                        "to": 1
                    }
                ])
        compare(libraryRow.currentIndex, 1)
        verify(libraryRow.routeKey(Qt.Key_Escape, "press", false))
        compare(libraryRow.moveMode, false)
        compare(libraryRow.beginDrag(1, 180, 20, 10, 20), false, "finishing returns to scrolling")
    }

    function test_moveEndsOnActivationOrModelChange() {
        verify(libraryRow.beginMoveById("bbbbbbbb:movies"))
        libraryRow.activate()
        compare(libraryRow.moveMode, false)
        verify(libraryRow.beginMove(0))
        const original = libraryRow.model
        libraryRow.model = []
        compare(libraryRow.moveMode, false)
        compare(libraryRow.currentIndex, -1)
        libraryRow.model = original
        compare(libraryRow.currentIndex, 0)
    }

    function test_emptyRowKeepsHiddenManagementAccessible() {
        const original = libraryRow.model
        libraryRow.headerAction = () => {
            ++testCase.headerActivations
            return true
        }
        libraryRow.reserveWhenEmpty = true
        libraryRow.model = []
        verify(libraryRow.rowVisible)
        verify(libraryRow.focusList())
        libraryRow.activate()
        compare(headerActivations, 1)
        verify(libraryRow.longPress())
        compare(headerActivations, 2)
        libraryRow.model = original
        libraryRow.reserveWhenEmpty = false
    }
    function test_libraryCardOmitsCollectionTypeSubtitle() {
        tryCompare(libraryRow, "count", 2)
        tryVerify(function () {
            return libraryRow.currentCard() !== null
        })
        compare(libraryRow.currentCard().titleText(), "Movies")
        compare(libraryRow.currentCard().subtitleText(), "movies")
        compare(libraryRow.currentCard().showSubtitle, false)
        compare(libraryRow.currentCard().emphasizedTitle, true)
    }

    function test_touchPressDoesNotMoveSelectionBeforeClick() {
        libraryRow.currentIndex = 0
        libraryRow.beginPointerSelection(1)
        compare(libraryRow.currentIndex, 0, "a scroll gesture must not briefly select the pressed card")
        compare(libraryRow.commitPointerSelection(), 1)
        compare(libraryRow.currentIndex, 1, "a completed tap should select the pressed card")
    }

    function test_touchOnlyModeHidesFocusRing() {
        libraryRow.keyboardFocusActive = false
        libraryRow.forceActiveFocus()
        tryVerify(function () {
            return libraryRow.currentCard() !== null
        })
        compare(libraryRow.currentCard().focused, false)
        libraryRow.keyboardFocusActive = true
    }
}
