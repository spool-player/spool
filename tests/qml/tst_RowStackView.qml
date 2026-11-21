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
}
