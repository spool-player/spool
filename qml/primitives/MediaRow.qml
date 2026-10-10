pragma ComponentBehavior: Bound

import QtQuick
import "../theme"
import "ModelAccess.js" as ModelAccess

FocusScope {
    id: root
    objectName: cardKind === "library" ? "libraryMediaRow" : "mediaRow"

    property string title: ""
    // Where the row, or each of its cards, comes from, when the page says so:
    // a map of iconUrl, text and (for a card) an optional detail line.
    property var headerBadge: null
    property var cardBadge: null
    property var model
    property var shell
    property string cardKind: "poster" // poster, square, landscape, library, or person
    property bool useSeriesPoster: false
    property bool preferEpisodeTitle: false
    property int cardWidth: Metrics.scaled(156)
    property int cardGap: Metrics.scaled(16)
    property int currentIndex: 0
    property bool enabledRow: true
    property bool reserveWhenEmpty: false
    property bool loading: false
    property string emptyText: "Loading..."
    property bool atomicPopulate: false
    property string itemContextSource: ""
    property string itemContextReturnRoute: ""
    property var wheelFlickable: null
    property bool focusVisible: true
    property bool keyboardFocusActive: Metrics.keyboardFocusActive
    property int pointerPressedIndex: -1
    // Reordering is opt-in; the host owns the model and persistence.
    property var moveItem: null
    readonly property bool reorderEnabled: typeof moveItem === "function"
    property var headerAction: null
    property string headerActionText: ""
    readonly property bool hasHeaderAction: typeof headerAction === "function"
    property var contextMenu: null
    property bool moveMode: false
    property bool publishingMove: false
    property bool dragActive: false
    property int dragSourceIndex: -1
    property int dragTargetIndex: -1
    property real dragPointerX: 0
    property real dragPointerY: 0
    property real dragOffsetX: 0
    property real dragOffsetY: 0
    property var dragCardData: ({})
    // Lets even the final card be positioned at the start of the viewport.
    property bool allowTrailingSpace: false
    property int modelRevision: 0
    readonly property bool delegatesPresented: presentation.delegatesReady

    readonly property int count: modelCount()
    readonly property bool rowVisible: enabledRow && (count > 0 || reserveWhenEmpty)
    readonly property bool posterCard: cardKind === "poster" || cardKind === "person"
    readonly property bool squareCard: cardKind === "square"
    readonly property real cardAspect: squareCard ? 1 : posterCard ? 1.5 : 9 / 16
    readonly property int headerHeight: Metrics.scaled(44)
    readonly property int cardHeight: Math.round(cardWidth * cardAspect + Metrics.scaled(60))
    readonly property int focusPadding: Math.max(2, Metrics.scaled(2))
    readonly property int pageStep: Math.max(1, Math.floor((listView.width + cardGap) / (cardWidth + cardGap)))
    readonly property real contentX: listView.contentX

    signal verticalWheelScrolled(var controller)
    signal pointerSelected
    signal activated(int index, var item)

    width: parent ? parent.width : implicitWidth
    height: rowVisible ? headerHeight + (count > 0 || loading || !hasHeaderAction ? Metrics.scaled(10) + cardHeight :
                                                                                    0) : 0
    implicitHeight: height
    visible: rowVisible
    onVisibleChanged: if (!visible && (moveMode || dragActive))
                          finishMove()
    onActiveFocusChanged: if (!activeFocus && !publishingMove && (moveMode || dragActive))
                              finishMove()
    focus: true

    Component.onCompleted: resetPresentation()
    onAtomicPopulateChanged: Qt.callLater(resetPresentation)
    onModelChanged: {
        ++modelRevision
        if (!publishingMove && (moveMode || dragActive))
            finishMove()
        if (!publishingMove)
            Qt.callLater(resetPresentation)
    }
    onCountChanged: {
        ++modelRevision
        if (!publishingMove) {
            currentIndex = count > 0 ? Math.max(0, Math.min(currentIndex, count - 1)) : -1
            if (moveMode || dragActive)
                finishMove()
        }
        // The view rewrites its currentIndex internally on model changes;
        // re-assert ours once it has processed them.
        Qt.callLater(syncViewCurrentIndex)
        if (!publishingMove)
            Qt.callLater(resetPresentation)
    }
    onCurrentIndexChanged: syncViewCurrentIndex()
    onReorderEnabledChanged: if (!reorderEnabled && (moveMode || dragActive))
                                 finishMove()

    // listView.currentIndex must never be a declarative binding: the view
    // writes the property itself (model resets, item removal), after which a
    // binding can sit stale — logical index and highlight then disagree until
    // the next property change. One-way imperative sync, re-asserted at every
    // interaction point, keeps the highlight truthful.
    function syncViewCurrentIndex() {
        const target = count > 0 ? Math.max(0, Math.min(currentIndex, count - 1)) : -1
        if (listView.currentIndex !== target)
            listView.currentIndex = target
    }

    Connections {
        target: root.model && root.model.rowCount !== undefined ? root.model : null
        ignoreUnknownSignals: true

        function onDataChanged() {
            ++root.modelRevision
        }
        function onModelReset() {
            ++root.modelRevision
            if (!root.publishingMove) {
                root.finishMove()
                root.currentIndex = root.count > 0 ? Math.max(0, Math.min(root.currentIndex, root.count - 1)) : -1
            }
            Qt.callLater(root.syncViewCurrentIndex)
        }
        function onRowsInserted() {
            ++root.modelRevision
        }
        function onRowsMoved() {
            ++root.modelRevision
        }
        function onRowsRemoved() {
            ++root.modelRevision
        }
    }

    function modelCount() {
        return ModelAccess.count(model)
    }

    // revision is unused but must stay in the signature: bindings pass
    // modelRevision so a model change re-evaluates them.
    function itemAt(index, revision) {
        return ModelAccess.at(model, index)
    }

    function focusList() {
        if (count <= 0) {
            if (hasHeaderAction)
                InputKeys.focus(headerButton)
            return hasHeaderAction
        }
        currentIndex = Math.max(0, Math.min(currentIndex, count - 1))
        syncViewCurrentIndex()
        InputKeys.focus(listView)
        return true
    }

    function topLeftVisibleCandidate(outerViewport) {
        return InputKeys.topLeftVisibleCandidate(listView, outerViewport)
    }

    function selectionVisiblyUsable(outerViewport) {
        if (headerButton.activeFocus)
            return true
        for (let index = 0; index < carouselButtons.children.length; ++index)
            if (carouselButtons.children[index].activeFocus)
                return true
        return InputKeys.selectionVisiblyUsable(listView, outerViewport)
    }

    function focusIndexWithoutScrolling(index) {
        if (!InputKeys.focusIndexWithoutScrolling(listView, index))
            return false
        currentIndex = index
        return true
    }

    function positionIndexAtStart(index) {
        if (count <= 0 || index < 0 || index >= count || listView.width <= 0)
            return false
        currentIndex = index
        syncViewCurrentIndex()
        listView.forceLayout()
        listView.positionViewAtIndex(index, ListView.Beginning)
        return true
    }

    function currentCard() {
        return listView.currentItem
    }

    function moveBy(delta) {
        if (count <= 0)
            return false
        currentIndex = Math.max(0, Math.min(count - 1, currentIndex + delta))
        // Covers the clamped no-op case too (already at an edge): the view
        // may still be showing a stale highlight that needs re-asserting.
        syncViewCurrentIndex()
        return true
    }

    function pageBy(direction) {
        if (moveBy(direction * pageStep))
            focusList()
    }

    function routeKey(key, phase, repeat) {
        if (moveMode) {
            if (InputKeys.isBack(key, false)) {
                if (phase !== "release")
                    finishMove()
                return true
            }
            if (phase !== "release") {
                if (key === Qt.Key_Left)
                    moveSelected(-1)
                else if (key === Qt.Key_Right)
                    moveSelected(1)
            }
            return true
        }
        if (phase === "release")
            return true
        if (key === Qt.Key_Left)
            return moveBy(-1)
        if (key === Qt.Key_Right)
            return moveBy(1)
        return false
    }

    function activateIndex(index) {
        if (hasHeaderAction && (count <= 0 || headerButton.activeFocus)) {
            headerAction()
            return
        }
        if (moveMode) {
            finishMove()
            return
        }
        if (index < 0 || index >= count)
            return
        currentIndex = index
        activated(index, itemAt(index))
    }

    function beginPointerSelection(index) {
        pointerPressedIndex = index
    }

    function commitPointerSelection() {
        const index = pointerPressedIndex
        pointerPressedIndex = -1
        if (index < 0)
            return -1
        pointerSelected()
        currentIndex = index
        return index
    }

    function activate() {
        activateIndex(currentIndex)
    }

    function longPress() {
        if (hasHeaderAction && (count <= 0 || headerButton.activeFocus))
            return Boolean(headerAction())
        if (cardKind === "person" || currentIndex < 0 || !shell)
            return false
        return openItemContext(currentIndex, currentCard(), true)
    }

    function openItemContext(index, anchor, deferBackdropDismissal) {
        if (!shell || index < 0 || index >= count)
            return false
        if (typeof contextMenu === "function")
            return Boolean(contextMenu(itemAt(index), anchor, {
                                           "row": root,
                                           "deferBackdropDismissal": Boolean(deferBackdropDismissal)
                                       }))
        if (cardKind === "library")
            return false
        return Boolean(shell.openItemMenu(itemAt(index), anchor, {
                                              "model": model,
                                              "index": index,
                                              "source": itemContextSource,
                                              "returnRoute": itemContextReturnRoute,
                                              "deferBackdropDismissal": Boolean(deferBackdropDismissal)
                                          }))
    }
    function libraryIndex(libraryId) {
        for (let index = 0; index < count; ++index)
            if (String(itemAt(index).libraryId || "") === libraryId)
                return index
        return -1
    }

    function beginMoveById(libraryId) {
        const index = libraryIndex(libraryId)
        return index >= 0 && beginMove(index)
    }

    function beginMove(index) {
        if (!reorderEnabled || index < 0 || index >= count)
            return false
        pointerPressedIndex = -1
        pointerSelected()
        currentIndex = index
        moveMode = true
        focusList()
        return true
    }

    function finishMove() {
        moveMode = false
        dragActive = false
        dragSourceIndex = -1
        dragTargetIndex = -1
        pointerPressedIndex = -1
        syncViewCurrentIndex()
    }

    function moveTo(from, to) {
        if (!moveMode || !reorderEnabled || from < 0 || to < 0 || from >= count || to >= count)
            return false
        const libraryId = String(itemAt(from).libraryId || "")
        const scrollOffset = listView.contentX - listView.originX
        publishingMove = true
        try {
            if (from !== to && !moveItem(from, to))
                return false
            const movedIndex = libraryIndex(libraryId)
            if (movedIndex < 0) {
                finishMove()
                return false
            }
            currentIndex = movedIndex
            syncViewCurrentIndex()
            listView.forceLayout()
            listView.contentX = listView.originX + scrollOffset
            listView.positionViewAtIndex(movedIndex, ListView.Contain)
            focusList()
            return true
        } finally {
            publishingMove = false
        }
    }

    function moveSelected(direction) {
        return moveTo(currentIndex, Math.max(0, Math.min(count - 1, currentIndex + direction)))
    }

    function beginDrag(index, x, y, pressX, pressY) {
        if (!moveMode || !reorderEnabled || index < 0 || index >= count)
            return false
        listView.cancelFlick()
        pointerSelected()
        currentIndex = index
        dragSourceIndex = index
        dragTargetIndex = index
        dragCardData = itemAt(index)
        const card = listView.itemAtIndex(index)
        dragOffsetX = card ? pressX + listView.contentX - card.x : cardWidth / 2
        dragOffsetY = pressY
        dragActive = true
        updateDrag(x, y)
        return true
    }

    function updateDrag(x, y) {
        dragPointerX = x
        dragPointerY = y
        // The nearest card stays a valid target even over gaps or an edge.
        const contentPosition = Math.max(0, Math.min(listView.width, x)) + listView.contentX - listView.originX
        dragTargetIndex = Math.max(0, Math.min(count - 1, Math.floor(contentPosition / (cardWidth + cardGap))))
    }

    function dropDrag() {
        const from = dragSourceIndex
        const to = dragTargetIndex
        dragActive = false
        dragSourceIndex = -1
        dragTargetIndex = -1
        pointerPressedIndex = -1
        moveTo(from, to)
    }

    function resetPresentation() {
        presentation.reset()
    }

    function schedulePresentation() {
        presentation.schedule()
    }

    AtomicViewReveal {
        id: presentation

        view: listView
        latencyMonitor: InputLatency
        enabled: root.atomicPopulate && root.count > 0 && listView.width > 0
        firstIndex: 0
        lastIndex: Math.min(root.count, Math.max(1, Math.ceil((listView.width + root.cardGap) / (root.cardWidth
                                                                                                 + root.cardGap)))) - 1
    }

    Component {
        id: cardDelegate

        MediaItemCard {
            id: card

            required property int index
            readonly property var cardData: root.itemAt(index, root.modelRevision)
            readonly property var cardItem: root.cardKind === "library" ? (cardData.item || ({})) : cardData
            readonly property bool libraryCard: root.cardKind === "library"
            readonly property bool personCard: root.cardKind === "person"
            readonly property var badge: root.cardBadge ? root.cardBadge(cardData) : null

            width: root.cardWidth
            height: listView.height
            shell: libraryCard || personCard ? null : root.shell
            kind: libraryCard ? "landscape" : personCard ? "poster" : root.cardKind
            item: cardItem || ({})
            titleOverride: libraryCard ? String(cardData.name || "") : personCard ? String(cardData.name || "") : ""
            subtitleOverride: libraryCard ? String(badge && badge.detail || cardData.collectionType || "") : personCard
                                            ? String(cardData.role || cardData.type || "") : ""
            showSubtitle: !libraryCard || Boolean(badge && badge.detail)
            badgeIcon: badge ? badge.iconUrl : ""
            badgeText: badge ? String(badge.text || "") : ""
            emphasizedTitle: libraryCard
            imageOverride: libraryCard ? Art.url(cardItem, "landscape") : personCard ? Art.url(cardItem, "poster") : ""
            fallbackIcon: libraryCard ? Theme.libraryIcon(cardData.collectionType) : personCard ? "person" : ""
            fallbackTint: libraryCard ? Theme.libraryTint(cardData.name) : "transparent"
            useSeriesPoster: root.useSeriesPoster
            preferEpisodeTitle: root.preferEpisodeTitle
            focused: root.keyboardFocusActive && root.focusVisible && card.index === listView.currentIndex
            moving: (root.moveMode && card.index === root.currentIndex) || (root.dragActive && card.index
                                                                            === root.dragTargetIndex)
            opacity: root.dragActive && card.index === root.dragSourceIndex ? 0.3 : 1
            artworkVisible: true

            Component.onCompleted: root.schedulePresentation()
            onArtworkReadyChanged: root.schedulePresentation()
        }
    }

    SectionHeader {
        id: rowHeader
        anchors.left: parent.left
        anchors.right: moveButtons.visible ? moveButtons.left : headerButton.visible ? headerButton.left : carouselButtons.visible
                                                                                       ? carouselButtons.left :
                                                                                         parent.right

        anchors.top: parent.top
        height: root.headerHeight
        title: root.moveMode ? "Move · Left/Right · OK or Back to finish" : root.title
        badgeIcon: root.headerBadge ? root.headerBadge.iconUrl : ""
        badgeText: root.headerBadge ? String(root.headerBadge.text || "") : ""
    }

    ActionButton {
        id: headerButton
        objectName: "showHiddenLibrariesButton"
        anchors.top: parent.top
        anchors.right: carouselButtons.visible ? carouselButtons.left : parent.right
        height: root.headerHeight
        visible: root.hasHeaderAction && !root.moveMode
        text: root.headerActionText
        iconName: "visibility"
        Accessible.role: Accessible.Button
        Accessible.name: text
        Accessible.onPressAction: root.headerAction()
        onClicked: {
            root.pointerSelected()
            root.headerAction()
        }
    }

    Row {
        id: carouselButtons
        anchors.top: parent.top
        anchors.right: parent.right
        height: root.headerHeight
        spacing: Metrics.scaled(4)
        visible: !root.moveMode && root.count > root.pageStep

        IconButton {
            width: root.headerHeight
            height: width
            iconName: "chevron_left"
            chromeless: true
            accessibleName: "Scroll " + root.title + " left"
            enabled: root.currentIndex > 0
            opacity: enabled ? 1 : 0.35
            onClicked: root.pageBy(-1)
        }

        IconButton {
            width: root.headerHeight
            height: width
            iconName: "chevron_right"
            chromeless: true
            accessibleName: "Scroll " + root.title + " right"
            enabled: root.currentIndex < root.count - 1
            opacity: enabled ? 1 : 0.35
            onClicked: root.pageBy(1)
        }
    }
    Row {
        id: moveButtons
        anchors.top: parent.top
        anchors.right: parent.right
        height: root.headerHeight
        spacing: Metrics.scaled(4)
        visible: root.moveMode

        IconButton {
            width: root.headerHeight
            height: width
            iconName: "chevron_left"
            accessibleName: "Move selected library left"
            focusOnClick: false
            Accessible.role: Accessible.Button
            Accessible.name: accessibleName
            Accessible.onPressAction: if (enabled)
                                          root.moveSelected(-1)
            enabled: root.currentIndex > 0
            opacity: enabled ? 1 : 0.35
            onClicked: root.moveSelected(-1)
        }

        IconButton {
            width: root.headerHeight
            height: width
            iconName: "chevron_right"
            accessibleName: "Move selected library right"
            focusOnClick: false
            enabled: root.currentIndex < root.count - 1
            Accessible.role: Accessible.Button
            Accessible.name: accessibleName
            Accessible.onPressAction: if (enabled)
                                          root.moveSelected(1)
            opacity: enabled ? 1 : 0.35
            onClicked: root.moveSelected(1)
        }

        ActionButton {
            width: implicitWidth
            height: root.headerHeight
            text: "Done"
            iconName: "check"
            Accessible.role: Accessible.Button
            Accessible.name: "Done moving " + root.title
            Accessible.onPressAction: {
                root.finishMove()
                root.focusList()
            }
            onClicked: {
                root.finishMove()
                root.focusList()
            }
        }
    }

    Timer {
        interval: 30
        repeat: true
        running: root.moveMode && root.dragActive
        onTriggered: {
            const edge = Math.min(Metrics.scaled(64), listView.width / 4)
            const direction = root.dragPointerX < edge ? -1 : root.dragPointerX > listView.width - edge ? 1 : 0
            if (!direction)
                return
            const minimum = listView.originX - listView.leftMargin
            const maximum = Math.max(minimum, listView.originX + listView.contentWidth - listView.width
                                     + listView.rightMargin)
            listView.contentX = Math.max(minimum, Math.min(maximum, listView.contentX + direction * Metrics.scaled(14)))
            root.updateDrag(root.dragPointerX, root.dragPointerY)
        }
    }

    ListView {
        id: listView
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: rowHeader.bottom
        anchors.topMargin: Metrics.scaled(10)
        height: root.cardHeight
        visible: root.count > 0
        opacity: root.delegatesPresented ? 1 : 0
        focus: true
        keyNavigationEnabled: false
        clip: true
        orientation: ListView.Horizontal
        flickableDirection: Flickable.HorizontalFlick
        acceptedButtons: Qt.LeftButton
        boundsBehavior: Flickable.StopAtBounds
        interactive: !root.moveMode
        flickDeceleration: Metrics.flickDecelerationPx
        maximumFlickVelocity: Metrics.maximumFlickVelocityPx
        spacing: root.cardGap
        cacheBuffer: root.atomicPopulate ? 0 : Math.round(root.cardWidth + root.cardGap)
        leftMargin: root.focusPadding
        rightMargin: root.allowTrailingSpace ? Math.max(root.focusPadding, width - root.cardWidth) : root.focusPadding
        reuseItems: true
        model: root.model
        delegate: cardDelegate

        highlightFollowsCurrentItem: true
        highlightMoveDuration: 16
        highlightResizeDuration: Theme.reducedMotion ? 0 : 75
        highlight: Item {
            z: 2
            Rectangle {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: listView.currentItem ? listView.currentItem.focusOutlineHeight : parent.height
                color: "transparent"
                radius: Math.max(0, Theme.radiusMedium - Theme.focusBorderWidth)
                border.width: Theme.focusBorderWidth
                border.color: root.keyboardFocusActive && root.focusVisible && listView.activeFocus ? Theme.accent :
                                                                                                      "transparent"
            }
        }

        Component.onCompleted: root.syncViewCurrentIndex()
        onCurrentIndexChanged: if (currentIndex >= 0)
                                   positionViewAtIndex(currentIndex, ListView.Contain)

        FastWheelHandler {
            id: wheelHandler
            enabled: root.wheelFlickable !== null
            flickable: root.wheelFlickable
            onScrolled: root.verticalWheelScrolled(wheelHandler)
        }

        Item {
            objectName: "mediaRowPointerArea"
            // Like GridPointerArea, observe taps passively on the viewport so
            // Flickable retains the drag and cancels activation past threshold.
            parent: listView
            width: listView.width
            height: listView.height
            z: 3

            TapHandler {
                enabled: !root.moveMode
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                gesturePolicy: TapHandler.DragThreshold
                longPressThreshold: 0.52
                property int pressedIndex: -1
                property bool pressedWhileMoving: false
                property bool held: false

                onPressedChanged: {
                    if (pressed) {
                        held = false
                        pressedWhileMoving = listView.moving
                        pressedIndex = listView.indexAt(point.position.x + listView.contentX, point.position.y
                                                        + listView.contentY)
                    } else if (held && root.shell) {
                        root.shell.finishItemMenuOpeningGesture()
                    }
                }
                onTapped: (eventPoint, button) => {
                    if (held || pressedWhileMoving || listView.moving || pressedIndex < 0)
                        return
                    root.pointerSelected()
                    root.currentIndex = pressedIndex
                    if (button === Qt.RightButton)
                        root.openItemContext(pressedIndex, listView.itemAtIndex(pressedIndex))
                    else
                        root.activateIndex(pressedIndex)
                }
                onLongPressed: {
                    if (pressedWhileMoving || listView.moving || pressedIndex < 0)
                        return
                    root.pointerSelected()
                    root.currentIndex = pressedIndex
                    held = root.openItemContext(pressedIndex, listView.itemAtIndex(pressedIndex), true)
                }
            }
        }

        MouseArea {
            id: pointerArea
            objectName: "mediaRowMovePointerArea"
            parent: listView
            enabled: root.moveMode
            property bool gestureConsumed: false
            property bool menuHeld: false
            property real pressX: 0
            property real pressY: 0
            anchors.fill: parent
            z: 3
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            pressAndHoldInterval: 520
            preventStealing: root.moveMode
            onPressed: mouse => {
                gestureConsumed = false
                menuHeld = false
                pressX = mouse.x
                pressY = mouse.y
                root.beginPointerSelection(listView.indexAt(mouse.x + listView.contentX, mouse.y + listView.contentY))
            }
            onPositionChanged: mouse => {
                if (!(pressedButtons & Qt.LeftButton) || !root.moveMode)
                    return
                const dx = Math.abs(mouse.x - pressX)
                const dy = Math.abs(mouse.y - pressY)
                if (!root.dragActive && dy >= Qt.styleHints.startDragDistance && dy > dx) {
                    // Leave vertical swipes to the containing homepage Flickable.
                    gestureConsumed = true
                    root.pointerPressedIndex = -1
                } else if (!root.dragActive && !gestureConsumed && dx >= Qt.styleHints.startDragDistance && dx >= dy) {
                    gestureConsumed = root.beginDrag(root.pointerPressedIndex, mouse.x, mouse.y, pressX, pressY)
                } else if (root.dragActive) {
                    root.updateDrag(mouse.x, mouse.y)
                }
            }
            onReleased: {
                if (root.dragActive)
                    root.dropDrag()
                if (menuHeld && root.shell)
                    root.shell.finishItemMenuOpeningGesture()
            }
            onCanceled: {
                root.dragActive = false
                root.dragSourceIndex = -1
                root.dragTargetIndex = -1
                root.pointerPressedIndex = -1
                if (menuHeld && root.shell)
                    root.shell.finishItemMenuOpeningGesture()
            }
            onClicked: mouse => {
                if (gestureConsumed) {
                    root.pointerPressedIndex = -1
                    return
                }
                const selectedIndex = root.commitPointerSelection()
                if (selectedIndex < 0)
                    return
                if (mouse.button === Qt.RightButton) {
                    root.openItemContext(selectedIndex, listView.itemAtIndex(selectedIndex))
                } else {
                    root.activateIndex(selectedIndex)
                }
            }
            onPressAndHold: {
                if (root.dragActive || root.pointerPressedIndex < 0)
                    return
                menuHeld = root.openItemContext(root.pointerPressedIndex, listView.itemAtIndex(root.pointerPressedIndex),
                                                true)
                gestureConsumed = menuHeld
            }
        }

        Loader {
            x: Math.max(0, Math.min(listView.width - width, root.dragPointerX - root.dragOffsetX))
            y: Math.max(0, Math.min(listView.height - height, root.dragPointerY - root.dragOffsetY))
            width: root.cardWidth
            height: root.cardHeight
            z: 4
            active: root.dragActive

            sourceComponent: MediaItemCard {
                readonly property bool libraryCard: root.cardKind === "library"
                readonly property var previewData: root.dragCardData
                readonly property var badge: root.cardBadge ? root.cardBadge(previewData) : null
                kind: libraryCard ? "landscape" : root.cardKind
                item: libraryCard ? previewData.item || ({}) : previewData
                titleOverride: libraryCard ? String(previewData.name || "") : ""
                subtitleOverride: libraryCard ? String(badge && badge.detail || "") : ""
                showSubtitle: !libraryCard || Boolean(badge && badge.detail)
                badgeIcon: badge ? badge.iconUrl : ""
                badgeText: badge ? String(badge.text || "") : ""
                emphasizedTitle: libraryCard
                fallbackIcon: libraryCard ? Theme.libraryIcon(previewData.collectionType) : ""
                fallbackTint: libraryCard ? Theme.libraryTint(previewData.name) : "transparent"
                useSeriesPoster: root.useSeriesPoster
                preferEpisodeTitle: root.preferEpisodeTitle
                moving: true
                opacity: 0.9
            }
        }
    }

    SecondaryText {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: rowHeader.bottom
        anchors.topMargin: Metrics.scaled(18)
        height: root.cardHeight
        visible: root.count <= 0 && root.reserveWhenEmpty
        text: root.loading ? root.emptyText : ""
        color: Theme.textMuted
        font.pixelSize: Metrics.metaSizePx
        verticalAlignment: Text.AlignTop
    }
}
