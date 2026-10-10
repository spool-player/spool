import QtQuick
import QtTest
import "../../qml/primitives" as Primitives
import "../../qml/browse" as Browse
import "../../qml/shell" as Shell

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

    ListModel {
        id: libraryOrderModel
    }

    Component {
        id: libraryOwnerComponent
        ListModel {}
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
            libraryOrderModel.move(from, to, 1)
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

    Component {
        id: rowStackComponent

        Shell.KeyRouter {
            property alias stack: stackView
            activeTarget: stackView
            platformMayPairHolds: false

            Browse.RowStackView {
                id: stackView
                anchors.fill: parent
            }
        }
    }

    function initTestCase() {
        initialModel = libraryRow.model
    }

    function init() {
        libraryRow.finishMove()
        libraryOrderModel.clear()
        for (let index = 0; index < initialModel.length; ++index)
            libraryOrderModel.append(initialModel[index])
        libraryRow.model = libraryOrderModel
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

    function test_offscreenLibraryRecoveryConsumesHoldUntilPhysicalRelease() {
        const router = createTemporaryObject(rowStackComponent, testCase, {
                                                 "width": testCase.width,
                                                 "height": testCase.height
                                             })
        verify(router)
        const cards = []
        for (let index = 0; index < 18; ++index)
            cards.push({
                           "name": "Library " + (index + 1),
                           "libraryId": "aaaaaaaa:library/" + index,
                           "collectionType": "movies",
                           "item": ({})
                       })
        router.stack.sections = [
                    {
                        "key": "libraries",
                        "title": "Libraries",
                        "kind": "library",
                        "model": cards
                    }
                ]
        tryVerify(() => router.stack.rowAt(0) !== null)
        const row = router.stack.rowAt(0)
        row.cardWidth = 156
        row.cardGap = 16
        verify(router.stack.focusPreferred("libraries"))
        row.currentIndex = 2
        tryVerify(() => row.currentCard() !== null && row.activeFocus)
        waitForRendering(row)
        const pointerSurface = findChild(row, "mediaRowPointerArea")
        verify(pointerSurface)
        const viewport = pointerSurface.parent
        // Real native ListView geometry after a horizontal scroll; there is no
        // synthetic pending flag or copied candidate/recovery implementation.
        viewport.contentX = viewport.originX + 6 * (row.cardWidth + row.cardGap)
        waitForRendering(row)
        compare(row.currentIndex, 2)
        const scrollX = viewport.contentX
        const scrollY = router.stack.contentY
        const event = {
            "modifiers": Qt.NoModifier,
            "text": ""
        }
        verify(router.dispatchNormalized(event, Qt.Key_Right, "press", false))
        compare(row.currentIndex, 6)
        compare(viewport.contentX, scrollX)
        compare(router.stack.contentY, scrollY)
        for (let repeat = 0; repeat < 20; ++repeat)
            verify(router.dispatchNormalized(event, Qt.Key_Right, "press", true))
        verify(router.dispatchNormalized(event, Qt.Key_Right, "release", true))
        verify(router.dispatchNormalized(event, Qt.Key_Right, "press", true))
        compare(row.currentIndex, 6)
        compare(viewport.contentX, scrollX)
        compare(router.stack.contentY, scrollY)
        verify(router.dispatchNormalized(event, Qt.Key_Right, "release", false))
        verify(router.dispatchNormalized(event, Qt.Key_Right, "press", false))
        compare(row.currentIndex, 7)
        verify(router.dispatchNormalized(event, Qt.Key_Right, "release", false))
    }

    function test_moveKeepsIdentityModeAndViewportThroughOrderPublication() {
        const owner = createTemporaryObject(libraryOwnerComponent, testCase)
        const router = createTemporaryObject(rowStackComponent, testCase, {
                                                 "width": testCase.width,
                                                 "height": testCase.height
                                             })
        verify(owner && router)
        for (let index = 0; index < 18; ++index)
            owner.append({
                             "name": "Library " + (index + 1),
                             "libraryId": "aaaaaaaa:library/" + index,
                             "collectionType": "movies",
                             "item": ({})
                         })
        function publishOrder() {
            const snapshot = []
            for (let index = 0; index < owner.count; ++index)
                snapshot.push(Object.assign({}, owner.get(index)))
            router.stack.sections = [
                        {
                            "key": "libraries",
                            "title": "Libraries",
                            "kind": "library",
                            "model": snapshot,
                            "moveItem": function (from, to) {
                                owner.move(from, to, 1)
                                return true
                            }
                        }
                    ]
        }
        owner.rowsMoved.connect(publishOrder)
        router.stack.measureFirstRow = true
        publishOrder()
        tryVerify(() => router.stack.rowAt(0) !== null)
        const row = router.stack.rowAt(0)
        row.cardWidth = 156
        row.cardGap = 16
        tryVerify(() => row.delegatesPresented)
        verify(router.stack.focusPreferred("libraries"))
        verify(row.beginMoveById("aaaaaaaa:library/7"))
        const viewport = findChild(row, "mediaRowPointerArea").parent
        viewport.contentX = viewport.originX + 6 * (row.cardWidth + row.cardGap)
        waitForRendering(row)
        const scrollX = viewport.contentX - viewport.originX
        const scrollY = router.stack.contentY
        const event = {
            "modifiers": Qt.NoModifier,
            "text": ""
        }
        verify(router.dispatchNormalized(event, Qt.Key_Right, "press", false))
        const movedRow = router.stack.currentRow()
        verify(movedRow)
        compare(movedRow.moveMode, true)
        compare(movedRow.currentIndex, 8)
        compare(movedRow.itemAt(movedRow.currentIndex).libraryId, "aaaaaaaa:library/7")
        compare(owner.get(8).libraryId, "aaaaaaaa:library/7")
        compare(viewport.contentX - viewport.originX, scrollX)
        compare(router.stack.contentY, scrollY)
        verify(router.dispatchNormalized(event, Qt.Key_Right, "release", false))
        waitForRendering(movedRow)
        compare(movedRow.moveMode, true)
        verify(movedRow.delegatesPresented)
        compare(movedRow.itemAt(movedRow.currentIndex).libraryId, "aaaaaaaa:library/7")
        movedRow.activate()
        compare(movedRow.moveMode, false)
        movedRow.model = []
        compare(movedRow.currentIndex, -1)
    }
}
