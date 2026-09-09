import QtQuick
import QtTest
import "../../qml/primitives" as Primitives

TestCase {
    id: testCase
    name: "NavGrid"
    width: 400
    height: 300
    when: windowShown

    property int modelSize: 100
    property double fakeNow: 1000

    Primitives.NavGrid {
        id: grid
        anchors.fill: parent
        cellWidth: 100
        cellHeight: 100
        model: testCase.modelSize
        holdTickInterval: 10000
        nowProvider: function () {
            return testCase.fakeNow
        }
        delegate: Rectangle {
            required property int index
            width: grid.cellWidth
            height: grid.cellHeight
        }
    }

    SignalSpy {
        id: holdStartedSpy
        target: grid
        signalName: "holdStarted"
    }

    SignalSpy {
        id: currentIndexSpy
        target: grid
        signalName: "currentIndexChanged"
    }

    ListModel {
        id: pagedModel
    }

    Primitives.NavGrid {
        id: pagedGrid
        width: 100
        height: 100
        visible: false
        cellWidth: 100
        cellHeight: 100
        model: pagedModel
        holdTickInterval: 10000
        nowProvider: function () {
            return testCase.fakeNow
        }
        delegate: Item {
            required property int index
            width: pagedGrid.cellWidth
            height: pagedGrid.cellHeight
        }
    }

    Primitives.NavGrid {
        id: fallbackGrid
        width: 300
        height: 100
        visible: false
        cellWidth: 100
        cellHeight: 100
        model: 30
        delegate: Item {
            required property int index
            width: fallbackGrid.cellWidth
            height: fallbackGrid.cellHeight
        }
    }

    Item {
        id: alternateFocus
        focus: true
    }

    function arm(target, key, heldMs, frameMs, accumulator) {
        target.heldKey = key
        target.holdStartedAt = fakeNow - heldMs
        target.lastHoldTickAt = fakeNow - frameMs
        target.holdAccumulator = accumulator || 0
        target.holdRepeatSeen = true
    }

    function init() {
        grid.stopAccelerating()
        pagedGrid.stopAccelerating()
        modelSize = 100
        grid.currentIndex = 0
        fakeNow = 1000
        grid.holdTraversalSeconds = 5
        grid.holdSpeedMultiplier = 1
        holdStartedSpy.clear()
        currentIndexSpy.clear()
        pagedModel.clear()
    }

    function test_visibleRecoveryUsesShiftedModelOrigin() {
        for (let index = 0; index < 300; ++index)
            pagedModel.append({
                                  "value": index
                              })
        pagedGrid.currentIndex = 0
        pagedGrid.forceLayout()
        pagedGrid.contentY = 12000
        pagedModel.remove(0, 100)
        pagedGrid.forceLayout()
        verify(pagedGrid.originY > 0, "removing preceding rows establishes the shifted-origin case")
        const visibleIndex = pagedGrid.indexAt(50, pagedGrid.contentY + 50)
        verify(visibleIndex >= 0)
        compare(pagedGrid.topLeftVisibleIndex(), visibleIndex,
                "focus recovery must refer to the visible model item, not its old layout offset")
    }
    function test_focusTransitionIsImmediate() {
        compare(grid.highlightMoveDuration, 0)
    }

    function test_singlePressMovesOneRowOnly() {
        verify(grid.routeKey(Qt.Key_Down, "press", false))
        compare(grid.currentIndex, 4)
        compare(grid.heldKey, Qt.Key_Down)
        wait(150)
        compare(grid.currentIndex, 4)
        verify(grid.routeKey(Qt.Key_Down, "release", false))
        compare(grid.heldKey, 0)
    }

    function test_singlePressNeverAcceleratesWithoutRepeat() {
        verify(grid.routeKey(Qt.Key_Down, "press", false))
        compare(grid.currentIndex, 4)
        fakeNow += grid.holdDelay + 100
        grid.accelerate()
        compare(grid.currentIndex, 4)
        compare(grid.holdRepeatSeen, false)
        verify(grid.routeKey(Qt.Key_Down, "release", false))
    }

    function test_tenUnmarkedPressesPerSecondRemainSingleSteps() {
        modelSize = 250
        for (let press = 1; press <= 10; ++press) {
            verify(grid.routeKey(Qt.Key_Down, "press", false))
            compare(grid.currentIndex, press * 4)
            compare(grid.holdRepeatSeen, false)
            fakeNow += 100
        }
        fakeNow += grid.holdDelay
        grid.accelerate()
        compare(grid.currentIndex, 40)
        compare(holdStartedSpy.count, 0)
        verify(grid.routeKey(Qt.Key_Down, "release", false))
    }

    function test_modelSizesKeepFirstPressPrecise_data() {
        return [
                    {
                        "tag": "one",
                        "size": 1,
                        "expected": 0
                    },
                    {
                        "tag": "ten",
                        "size": 10,
                        "expected": 4
                    },
                    {
                        "tag": "two-hundred-fifty",
                        "size": 250,
                        "expected": 4
                    },
                    {
                        "tag": "five-thousand",
                        "size": 5000,
                        "expected": 4
                    }
                ]
    }

    function test_modelSizesKeepFirstPressPrecise(data) {
        modelSize = data.size
        grid.currentIndex = 0
        verify(grid.routeKey(Qt.Key_Down, "press", false))
        compare(grid.currentIndex, data.expected)
    }

    function test_kodiAccelerationCurveScalesWithLibrary() {
        modelSize = 1
        compare(grid.holdMaximumRate, 30)
        modelSize = 10
        compare(grid.holdMaximumRate, 30)
        modelSize = 250
        compare(grid.holdMaximumRate, 250 / 5)
        modelSize = 5000
        compare(grid.holdMaximumRate, 5000 / 5)
        compare(grid.holdInitialRate, 40)
        compare(grid.accelerationRate(grid.holdDelay - 1), 0)
        compare(grid.accelerationRate(grid.holdDelay), grid.holdInitialRate)
        compare(grid.accelerationRate(grid.holdDelay + grid.holdCruiseDuration), grid.holdInitialRate)
        verify(grid.accelerationRate(grid.holdDelay + grid.holdCruiseDuration + 500) > grid.holdInitialRate)
        compare(grid.accelerationRate(grid.holdDelay + grid.holdCruiseDuration + grid.holdRampDuration),
                grid.holdMaximumRate)
    }

    function test_holdSpeedMultiplierScalesWholeCurve() {
        modelSize = 5000
        const timestamps = [grid.holdDelay, grid.holdDelay + grid.holdCruiseDuration, grid.holdDelay
                            + grid.holdCruiseDuration + grid.holdRampDuration / 2, grid.holdDelay
                            + grid.holdCruiseDuration + grid.holdRampDuration]
        const fullRates = timestamps.map(timestamp => grid.accelerationRate(timestamp))

        grid.holdSpeedMultiplier = 0.5
        compare(grid.holdInitialRate, 20)
        compare(grid.holdMaximumRate, 500)
        for (let index = 0; index < timestamps.length; ++index)
            compare(grid.accelerationRate(timestamps[index]), fullRates[index] * 0.5)
    }

    function test_largeLibraryMaximumTraversesInThreePointFiveSeconds() {
        modelSize = 250
        grid.holdTraversalSeconds = 3.5
        compare(grid.holdMaximumRate, 250 / 3.5)
        compare(modelSize / grid.holdMaximumRate, 3.5)
    }

    function test_autoRepeatReleasePreservesAccelerationRamp() {
        modelSize = 250
        grid.currentIndex = 0
        verify(grid.routeKey(Qt.Key_Down, "press", false))
        compare(grid.currentIndex, 4)
        fakeNow += 500
        verify(grid.routeKey(Qt.Key_Down, "release", true))
        compare(grid.heldKey, Qt.Key_Down)
        verify(grid.routeKey(Qt.Key_Down, "press", true))
        compare(grid.currentIndex, 8)
        const startedAt = grid.holdStartedAt
        compare(grid.accelerationRate(grid.holdCruiseDuration - 1), grid.holdInitialRate)
        fakeNow += grid.holdCruiseDuration + 100
        verify(grid.routeKey(Qt.Key_Down, "release", true))
        compare(grid.heldKey, Qt.Key_Down)
        verify(grid.routeKey(Qt.Key_Down, "press", true))
        compare(grid.holdStartedAt, startedAt)
        verify(grid.accelerationRate(fakeNow - startedAt) > grid.holdInitialRate)
        grid.accelerate()
        verify(grid.currentIndex > 8)
        verify(grid.routeKey(Qt.Key_Down, "release", false))
        compare(grid.heldKey, 0)
        const stoppedAt = grid.currentIndex
        fakeNow += 500
        grid.accelerate()
        compare(grid.currentIndex, stoppedAt)
    }

    function test_fractionalMovementUsesElapsedTime() {
        modelSize = 250
        grid.currentIndex = 0
        arm(grid, Qt.Key_Down, 3000, 10, 0)
        grid.accelerate()
        compare(grid.currentIndex, 0)
        verify(grid.holdAccumulator > 0)
        fakeNow += 20
        grid.accelerate()
        compare(grid.currentIndex, 4)
    }

    function test_frameStallsClampToFiftyMilliseconds() {
        modelSize = 5000
        grid.currentIndex = 0
        fakeNow = 5000
        arm(grid, Qt.Key_Down, 3000, 1000, 0)
        grid.accelerate()
        compare(grid.currentIndex, 200)
        compare(grid.holdAccumulator, 0)
    }

    function test_acceleratedTickCoalescesIndexChange() {
        modelSize = 5000
        grid.currentIndex = 0
        currentIndexSpy.clear()
        fakeNow = 5000
        arm(grid, Qt.Key_Down, 3000, 50, 0)
        grid.accelerate()
        compare(grid.currentIndex, 200)
        compare(currentIndexSpy.count, 1)
    }

    function test_directionReversalAndReleaseResetAccumulator() {
        arm(grid, Qt.Key_Down, 800, 20, 0.75)
        verify(grid.routeKey(Qt.Key_Left, "press", true))
        compare(grid.heldKey, Qt.Key_Left)
        compare(grid.holdAccumulator, 0)
        verify(grid.routeKey(Qt.Key_Left, "release", false))
        compare(grid.heldKey, 0)
        compare(grid.holdAccumulator, 0)
    }

    function test_focusZoneChangeStopsAcceleration() {
        grid.forceActiveFocus()
        arm(grid, Qt.Key_Down, 800, 20, 0.75)
        alternateFocus.forceActiveFocus()
        tryCompare(grid, "heldKey", 0)
        compare(grid.holdAccumulator, 0)
    }

    function test_modelReplacementStopsAcceleration() {
        arm(grid, Qt.Key_Down, 800, 20, 0.75)
        modelSize = 10
        tryCompare(grid, "heldKey", 0)
        compare(grid.holdAccumulator, 0)
    }

    function test_paginationAppendContinuesHeldTraversal() {
        for (let index = 0; index < 10; ++index)
            pagedModel.append({
                                  "value": index
                              })
        pagedGrid.currentIndex = 9
        arm(pagedGrid, Qt.Key_Down, 3000, 50, 1)
        pagedGrid.accelerate()
        compare(pagedGrid.currentIndex, 9)
        compare(pagedGrid.heldKey, Qt.Key_Down)
        pagedModel.append({
                              "value": 10
                          })
        fakeNow += 50
        pagedGrid.lastHoldTickAt = fakeNow - 50
        pagedGrid.holdAccumulator = 1
        pagedGrid.accelerate()
        compare(pagedGrid.currentIndex, 10)
    }

    function test_accelerationStopsWhenTvOmitsReleaseEvent() {
        grid.forceActiveFocus()
        verify(grid.routeKey(Qt.Key_Down, "press", false))
        verify(grid.routeKey(Qt.Key_Down, "press", true))
        compare(grid.heldKey, Qt.Key_Down)
        tryCompare(grid, "heldKey", 0, grid.holdReleaseTimeout + 1000)
    }

    function test_topLeftVisibleIndexEmpty() {
        modelSize = 0
        compare(grid.topLeftVisibleIndex(), -1)
    }

    function test_topLeftVisibleIndexUsesVisibilityThreshold() {
        modelSize = 100
        grid.forceLayout()
        grid.contentY = 15
        compare(grid.topLeftVisibleIndex(), 0)
        grid.contentY = 45
        compare(grid.topLeftVisibleIndex(), 4)
        grid.contentY = 115
        compare(grid.topLeftVisibleIndex(), 4)
        grid.contentY = 145
        compare(grid.topLeftVisibleIndex(), 8)
    }

    function test_topLeftVisibleIndexFallsBackWithoutDelegate() {
        fallbackGrid.contentY = 145
        compare(fallbackGrid.topLeftVisibleIndex(), 6)
    }
}
