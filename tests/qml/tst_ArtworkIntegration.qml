import QtQuick
import QtTest
import Spool

TestCase {
    id: testCase
    name: "ArtworkIntegration"
    width: 1000
    height: 600
    visible: true
    when: windowShown
    property var currentModel: null

    MediaRow {
        id: row
        width: testCase.width
        model: testCase.currentModel
        cardKind: "landscape"
        cardWidth: Math.round(testCase.width / 4)
    }
    function imageCard(item) {
        if (typeof item.artworkSource === "function")
            return item
        for (const child of item.children || []) {
            const found = imageCard(child)
            if (found)
                return found
        }
        return null
    }
    function verifyPicture() {
        let picture = null
        tryVerify(() => {
            picture = imageCard(row)
            return picture && picture.artworkReady && !picture.showingFallback
        })
        waitForRendering(picture)
        const center = picture.mapToItem(testCase, picture.width / 2, picture.height / 2)
        const pixels = grabImage(testCase)
        const x = Math.floor(center.x)
        const y = Math.floor(center.y)
        verify(pixels.green(x, y) > 140, "The requested image must render, not a transparent success or placeholder")
        verify(pixels.red(x, y) < 60)
    }
    function init() {
        currentModel = null
        testCase.width = 1000
        row.cardKind = "landscape"
        wait(10)
    }
    function test_firstDataAndResize() {
        currentModel = ArtworkModel
        verifyPicture()
        testCase.width = 700
        verifyPicture()
        testCase.width = 1100
        verifyPicture()
    }
    function test_scriptItemsAndRecreatedRows() {
        const item = JSON.parse(JSON.stringify(ArtworkModel.get(0)))
        currentModel = [item]
        verifyPicture()
        testCase.width = 650
        currentModel = null
        wait(10)
        currentModel = [Object.assign({}, item)]
        verifyPicture()
        row.cardKind = "poster"
        verifyPicture()
        testCase.width = 1050
        verifyPicture()
    }
    function test_scriptItemsPreserveArtworkOwner() {
        const data = JSON.parse(JSON.stringify(ArtworkModel.get(0)))
        for (const kind of ["landscape", "backdrop", "seriesPoster"]) {
            const url = Art.url(data, kind)
            verify(url.indexOf("green.png") >= 0, kind + " must retain its image tag and owner")
            compare(url, Art.url(ArtworkModel.get(0), kind))
        }
    }
}
