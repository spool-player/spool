pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme"
import "../primitives"
import "../browse"
import "../shell/ItemActivation.js" as ItemActivation

FocusScope {
    id: root

    property var shell
    readonly property var search: Search
    readonly property string query: search ? search.query : ""
    readonly property int resultCount: search ? search.resultCount : 0
    readonly property int suggestionCount: search && search.suggestions ? search.suggestions.count : 0
    readonly property bool searchBusy: search ? search.busy : false
    readonly property bool suggestionsBusy: search ? search.suggestionsBusy : false
    readonly property bool showSuggestions: query.length < 2 && suggestionCount > 0
    readonly property var resultSections: [
        {
            "key": "movies",
            "title": "Movies",
            "model": search ? search.movieResults : null
        },
        {
            "key": "series",
            "title": "Series",
            "model": search ? search.seriesResults : null
        },
        {
            "key": "episodes",
            "title": "Episodes",
            "model": search ? search.episodeResults : null,
            "useSeriesPoster": true,
            "preferEpisodeTitle": true
        },
        {
            "key": "other",
            "title": "More",
            "model": search ? search.otherResults : null
        }
    ]
    // Result kind the user last interacted with; picks the row to focus
    // when results rebuild. Page-local — the page is resident.
    property string preferredKind: "movies"

    focus: true

    Component.onCompleted: {
        if (search)
            search.loadSuggestions()
        field.text = query
        field.focusField()
    }

    Connections {
        target: root.search

        function onQueryChanged() {
            if (field.text !== root.query)
                field.text = root.query
        }
        function onResultsChanged() {
            root.repairResultFocus()
        }
    }

    function repairResultFocus() {
        if (!results.repair() && results.activeFocus)
            field.focusField()
    }

    function activateResult(section, index, item) {
        if (!section)
            return
        preferredKind = String(section.key || "")
        ItemActivation.open(item, {
                                "source": "search",
                                "returnRoute": "search",
                                "browseRoute": "libraryGrid"
                            }, App, shell, section.model, index)
    }

    function setQuery(text) {
        if (search)
            search.setQuery(text)
    }

    function routeKey(key, phase, repeat) {
        if (suggestionsRow.activeFocus) {
            if (key === Qt.Key_Up) {
                field.focusField()
                return true
            }
            if (key === Qt.Key_Down)
                return true
            return suggestionsRow.routeKey(key, phase, repeat)
        }

        if (results.activeFocus)
            return results.routeKey(key, phase, repeat)

        if (key === Qt.Key_Up && field.activeFocus && !field.editing) {
            if (shell)
                shell.focusNavBar()
            return true
        }
        if (key !== Qt.Key_Down || !(field.activeFocus || field.editing))
            return false
        if (query.length >= 2)
            return results.focusPreferred(preferredKind)
        return showSuggestions && suggestionsRow.focusList()
    }

    function activateSuggestion(index) {
        if (index < 0)
            return
        ItemActivation.open(search.suggestions.get(index), {
                                "source": "suggestion",
                                "returnRoute": "search",
                                "browseRoute": ""
                            }, App, shell, search.suggestions, index)
    }

    function activate() {
        if (field.activeFocus && !field.editing) {
            field.activate()
            return
        }
        if (suggestionsRow.activeFocus) {
            activateSuggestion(suggestionsRow.currentIndex)
            return
        }
        results.activate()
    }

    function currentMediaItem() {
        if (suggestionsRow.activeFocus && suggestionsRow.currentIndex >= 0)
            return suggestionsRow.itemAt(suggestionsRow.currentIndex)
        return results.activeFocus ? results.currentItem() : ({})
    }

    function longPress() {
        return results.activeFocus && results.longPress()
    }

    function back() {
        if (!suggestionsRow.activeFocus && !results.activeFocus)
            return false
        field.focusField()
        return true
    }

    readonly property real keyboardInset: {
        if (!Qt.inputMethod.visible)
            return 0
        const keyboard = Qt.inputMethod.keyboardRectangle
        if (keyboard.height <= 0)
            return 0
        const pageBottom = root.mapToItem(null, 0, root.height).y
        return Math.max(0, Math.min(root.height, pageBottom - keyboard.y))
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Metrics.pageMarginPx
        anchors.bottomMargin: Metrics.pageMarginPx + root.keyboardInset
        spacing: Metrics.gapPx

        Behavior on anchors.bottomMargin {
            enabled: !Theme.reducedMotion
            NumberAnimation {
                duration: 120
                easing.type: Easing.OutCubic
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Metrics.scaled(68)

            TextFieldRow {
                id: field

                anchors.fill: parent
                placeholderText: "Search everything"
                inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                enterKeyType: Qt.EnterKeySearch
                onTextEdited: text => root.setQuery(text)
                onAccepted: if (root.search)
                                root.search.submit()
            }

            BusySpinner {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.rightMargin: Metrics.scaled(18)
                width: Metrics.scaled(28)
                height: width
                running: root.searchBusy
                visible: running
            }
        }

        MediaRow {
            id: suggestionsRow

            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            title: "Suggestions"
            model: root.search ? root.search.suggestions : null
            shell: root.shell
            cardWidth: Metrics.cardWidth(root.width)
            cardGap: Metrics.gapPx
            enabledRow: root.query.length < 2
            reserveWhenEmpty: root.query.length < 2 && root.suggestionsBusy
            loading: root.suggestionsBusy
            emptyText: "Loading suggestions..."
            visible: root.query.length < 2
            onActivated: index => root.activateSuggestion(index)
        }

        EmptyPlaceholder {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.query.length < 2 && !root.showSuggestions && !root.suggestionsBusy
            title: "Start typing"
            detail: "Enter at least two characters."
        }

        RowStackView {
            id: results

            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.query.length >= 2
            sections: root.resultSections
            shell: root.shell
            contextReturnRoute: "search"
            rowSpacing: Metrics.sectionGapPx

            // Coming back to results should land on the kind you were last
            // looking at rather than always on Movies.
            onCurrentSectionChanged: {
                const section = sectionAt(currentSection)
                if (section && activeFocus)
                    root.preferredKind = String(section.key || "")
            }
            onEdgeUp: field.focusField()
            onActivated: (section, index, item) => root.activateResult(section, index, item)

            footer: Item {
                width: results.width
                height: results.height
                visible: root.resultCount === 0 && !root.searchBusy

                AppText {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.verticalCenter: parent.verticalCenter
                    text: "No Results"
                    font.pixelSize: Metrics.bodySizePx + Metrics.scaled(10)
                    font.weight: Font.DemiBold
                }
            }
        }
    }
}
