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
    QtObject {
        id: browse
        property var items: ArtworkModel
        property string libraryCollectionType: "movies"
        property var query: ({})
        property var filterOptions: ({})
        property int filterActiveCount: 0
        property bool loadingMore: false
        property string viewKind: "library"
        property string libraryId: "library"
        property string title: "Movies"
        property int totalCount: 300
        signal changed
        function prefetchVisibleRange(first, last) {
        }
        function prefetchPageForIndex(index) {
        }
        function mediaInfoFor(index, language) {
            return null
        }
    }
    QtObject {
        id: libraries
        function rowCount() {
            return 0
        }
    }
    QtObject {
        id: nativeWindow
        property real systemMemoryBytes: 0
    }
    QtObject {
        id: settings
        property var values: ({})
        property int uiScalePercent: 100
    }
    function findGrid(item) {
        if (typeof item.positionViewAtIndex === "function")
            return item
        for (const child of item.children || []) {
            const found = findGrid(child)
            if (found)
                return found
        }
        return null
    }
    function test_zLibraryScrollLoadsViewportOnly() {
        ArtworkFixture.prepareLibrary(browse, libraries, nativeWindow, settings)
        const component = Qt.createComponent("qrc:/qt/qml/Spool/qml/pages/LibraryGridPage.qml")
        compare(component.status, Component.Ready, component.errorString())
        const page = createTemporaryObject(component, testCase, {
                                               width: 1000,
                                               height: 600
                                           })
        verify(page)
        const grid = findGrid(page)
        verify(grid)
        tryVerify(() => grid.cacheBuffer > 0)
        tryVerify(() => ArtworkFixture.requestedItems.length > 0)
        wait(150)
        const topTail = Math.ceil(grid.height / grid.cellHeight) * page.columns
        for (const id of ArtworkFixture.requestedItems)
            verify(Number(id.split("library-")[1]) < topTail, "Buffered posters must not delay visible images: " + id)

        grid.positionViewAtIndex(240, GridView.Beginning)
        grid.forceLayout()
        tryVerify(() => ArtworkFixture.requestedItems.indexOf("01234567:library-240") >= 0)
        const tail = 240 + Math.ceil(grid.height / grid.cellHeight) * page.columns
        for (const id of ArtworkFixture.requestedItems) {
            const index = Number(id.split("library-")[1])
            verify(index < topTail || (index >= 240 && index < tail),
                   "Scrolling must load the new viewport, not intervening rows: " + id)
        }
        const picture = imageCard(grid.itemAtIndex(240))
        tryVerify(() => picture && picture.artworkReady && !picture.showingFallback)
        waitForRendering(picture)
        const pixels = grabImage(picture)
        verify(pixels.green(Math.floor(picture.width / 2), Math.floor(picture.height / 2)) > 140)
    }
}
