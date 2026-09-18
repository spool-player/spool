import QtQuick
import QtTest
import "../../qml/providers/jellyfin/ProfileNavigation.js" as ProfileNavigation

TestCase {
    name: "ProfileNavigation"

    // Three accounts plus the add tile, two to a row.
    function test_walksTheRunAcrossRows() {
        compare(ProfileNavigation.move(0, 3, 2, "right"), 1)
        compare(ProfileNavigation.move(1, 3, 2, "right"), 2)
        compare(ProfileNavigation.move(3, 3, 2, "right"), 3)
        compare(ProfileNavigation.move(2, 3, 2, "left"), 1)
    }

    function test_stepsByRow() {
        compare(ProfileNavigation.move(0, 3, 2, "down"), 2)
        compare(ProfileNavigation.move(2, 3, 2, "up"), 0)
        compare(ProfileNavigation.move(3, 3, 2, "down"), 3)
    }

    function test_shortLastRowStillReachesTheAddTile() {
        // Four accounts over three columns: the second row holds two cells, so
        // down from the third column has to fall back to the last of them.
        compare(ProfileNavigation.move(2, 4, 3, "down"), 4)
    }

    function test_leavingTheGridReportsItself() {
        compare(ProfileNavigation.move(0, 3, 2, "up"), -1)
        compare(ProfileNavigation.move(1, 3, 2, "up"), -1)
        compare(ProfileNavigation.move(0, 3, 2, "left"), -1)
    }

    function test_addTileAloneIsTheWholeGrid() {
        compare(ProfileNavigation.move(0, 0, 4, "right"), 0)
        compare(ProfileNavigation.move(0, 0, 4, "down"), 0)
        compare(ProfileNavigation.move(0, 0, 4, "up"), -1)
    }

    function test_outOfRangeIndexesAreClamped() {
        compare(ProfileNavigation.move(99, 3, 2, "left"), 2)
        compare(ProfileNavigation.move(-4, 3, 2, "right"), 1)
    }
}
