// Requires an application-aware fixture supplying the theme and native
// singletons used by MediaRow. The stock qmltestrunner CTest launcher does
// not provide those services, so this scenario is not registered there.
import QtQuick
import QtTest
import "../../qml/browse" as Browse

TestCase {
    id: testCase
    name: "RowStackView"
    width: 640
    height: 900
    when: windowShown

    Component {
        id: stackComponent
        Browse.RowStackView {
            width: 640
            height: 900
            loading: true
            sections: [
                {
                    "key": "populated",
                    "model": [
                        {
                            "title": "First"
                        },
                        {
                            "title": "Second"
                        }
                    ],
                    "reserveWhileLoading": true
                },
                {
                    "key": "pending",
                    "model": [],
                    "reserveWhileLoading": true
                }
            ]
        }
    }

    SignalSpy {
        id: sectionsSpy
        signalName: "sectionsChanged"
    }

    SignalSpy {
        id: activatedSpy
        signalName: "activated"
    }

    function latestSection(libraryId, itemId, position) {
        return {
            "key": "latestLibrary:" + libraryId,
            "model": [
                {
                    "id": itemId,
                    "title": itemId
                },
                {
                    "id": itemId + "-second",
                    "title": itemId + "-second"
                }
            ],
            "contextSource": "latestLibrary:" + position
        }
    }

    function test_loadingPreservesRowsAndHorizontalSelection() {
        const stack = createTemporaryObject(stackComponent, testCase)
        verify(stack)
        tryVerify(function () {
            return stack.rowAt(0) !== null && stack.rowAt(1) !== null
        })
        const populated = stack.rowAt(0)
        const pending = stack.rowAt(1)
        populated.currentIndex = 1
        compare(pending.rowVisible, true)
        sectionsSpy.target = stack
        sectionsSpy.clear()

        stack.loading = false
        tryCompare(pending, "rowVisible", false)
        compare(stack.rowAt(0), populated)
        compare(populated.currentIndex, 1)
        compare(sectionsSpy.count, 0)

        stack.loading = true
        tryCompare(pending, "rowVisible", true)
        compare(stack.rowAt(0), populated)
        compare(populated.currentIndex, 1)
        compare(sectionsSpy.count, 0)
        sectionsSpy.target = null
    }

    function test_explicitReservationDoesNotDependOnLoading() {
        const stack = createTemporaryObject(stackComponent, testCase, {
                                                "loading": false,
                                                "sections": [
                                                    {
                                                        "model": [],
                                                        "reserveWhenEmpty": true
                                                    }
                                                ]
                                            })
        verify(stack)
        tryVerify(function () {
            return stack.rowAt(0) !== null
        })
        compare(stack.rowAt(0).rowVisible, true)
    }

    function test_latestRowInsertedAheadKeepsSelectedRowActivatable() {
        const stack = createTemporaryObject(stackComponent, testCase, {
                                                "height": 700,
                                                "sections": [latestSection("aaaa:movies", "movies0", 0), latestSection(
                                                        "bbbb:series", "series0", 1)]
                                            })
        verify(stack)
        tryVerify(() => stack.rowAt(1) !== null)
        verify(stack.focusSection(1))
        stack.currentRow().currentIndex = 1
        waitForRendering(stack)
        activatedSpy.target = stack
        activatedSpy.clear()
        compare(stack.currentItem().id, "series0-second")
        const screenY = stack.currentRow().mapToItem(stack, 0, 0).y

        stack.sections = [latestSection("cccc:movies", "new0", 0), latestSection("aaaa:movies", "movies0", 1), latestSection("bbbb:series",
                                                                                                                             "series0",
                                                                                                                             2)]
        waitForRendering(stack)
        tryVerify(function () {
            return stack.count === 3 && stack.currentRow() !== null
        })

        verify(stack.activeFocus, "the stack keeps keyboard focus through the rebuild")
        compare(stack.currentSection, 2, "the anchor must follow the key, not the index the row used to have")
        compare(stack.currentItem().id, "series0-second", "late shelves must not change the selected card")
        verify(Math.abs(stack.currentRow().mapToItem(stack, 0, 0).y - screenY) < 1,
               "late shelves must not move the focused card on screen")

        stack.activate()
        tryCompare(activatedSpy, "count", 1)
        compare(activatedSpy.signalArguments[0][0].key, "latestLibrary:bbbb:series")
        compare(activatedSpy.signalArguments[0][1], 1)
        compare(activatedSpy.signalArguments[0][2].id, "series0-second")
        activatedSpy.target = null
    }
}
