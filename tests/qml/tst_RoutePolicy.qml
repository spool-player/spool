import QtQuick
import QtTest
import "../../qml/shell/RoutePolicy.js" as RoutePolicy

TestCase {
    name: "RoutePolicy"

    function modelFor(items) {
        return {
            values: items,
            rowCount: function () {
                return this.values.length
            },
            get: function (index) {
                return this.values[index]
            }
        }
    }

    function test_detailsSources_data() {
        return [
                    {
                        tag: "library",
                        source: "movies",
                        returnRoute: "libraryGrid"
                    },
                    {
                        tag: "search",
                        source: "search",
                        returnRoute: "search"
                    },
                    {
                        tag: "latest",
                        source: "latest",
                        returnRoute: "home"
                    },
                    {
                        tag: "resume",
                        source: "resume",
                        returnRoute: "home"
                    },
                    {
                        tag: "person",
                        source: "person",
                        returnRoute: "personDetails"
                    },
                    {
                        tag: "genre",
                        source: "genre",
                        returnRoute: "libraryGrid"
                    },
                    {
                        tag: "studio",
                        source: "studio",
                        returnRoute: "libraryGrid"
                    },
                    {
                        tag: "similar",
                        source: "similar",
                        returnRoute: "itemDetails"
                    }
                ]
    }

    function test_detailsSources(data) {
        const model = modelFor([
                                   {
                                       movieId: "first",
                                       itemType: "Movie"
                                   },
                                   {
                                       movieId: "selected",
                                       itemType: "Episode"
                                   }
                               ])
        const route = RoutePolicy.detailsRouteAt(model, 1, data.source, data.returnRoute, "home")

        verify(route !== null)
        compare(route.itemId, "selected")
        compare(route.itemType, "Episode")
        compare(route.source, data.source)
        compare(route.returnRoute, data.returnRoute)
        compare(route.focusIndex, 1)

        model.values = [
                    {
                        movieId: "selected",
                        itemType: "Episode"
                    },
                    {
                        movieId: "first",
                        itemType: "Movie"
                    }
                ]
        compare(RoutePolicy.modelIndexForItemId(model, route.itemId, route.focusIndex), 0)
    }

    function test_missingItemRejected() {
        const model = modelFor([])
        compare(RoutePolicy.detailsRouteAt(model, 0, "search", "search", "home"), null)
    }

    function test_fallbackIndexIsBounded() {
        const model = modelFor([
                                   {
                                       movieId: "only"
                                   }
                               ])
        // A route's identity never falls back to a different row: a missing
        // item stays missing. Only a route without an id uses the clamped
        // fallback index.
        compare(RoutePolicy.modelIndexForItemId(model, "missing", 9), -1)
        compare(RoutePolicy.modelIndexForItemId(modelFor([]), "missing", 9), -1)
        compare(RoutePolicy.modelIndexForItemId(model, "", 9), 0)
        compare(RoutePolicy.modelIndexForItemId(model, "", -5), 0)
    }

    function test_detailsNavigationMode_data() {
        return [
                    {
                        tag: "from home",
                        route: "home",
                        currentArgs: {},
                        nextArgs: {
                            itemId: "episode"
                        },
                        source: "latest",
                        mode: "push"
                    },
                    {
                        tag: "episode to its series",
                        route: "itemDetails",
                        currentArgs: {
                            itemId: "episode"
                        },
                        nextArgs: {
                            itemId: "series"
                        },
                        source: "series-link",
                        mode: "push"
                    },
                    {
                        tag: "similar item",
                        route: "itemDetails",
                        currentArgs: {
                            itemId: "movie"
                        },
                        nextArgs: {
                            itemId: "other-movie"
                        },
                        source: "similar",
                        mode: "push"
                    },
                    {
                        tag: "same item again",
                        route: "itemDetails",
                        currentArgs: {
                            itemId: "movie"
                        },
                        nextArgs: {
                            itemId: "movie"
                        },
                        source: "resume",
                        mode: "replace"
                    },
                    {
                        tag: "back from playback",
                        route: "itemDetails",
                        currentArgs: {
                            itemId: "episode"
                        },
                        nextArgs: {
                            itemId: "next-episode"
                        },
                        source: "playback",
                        mode: "replace"
                    }
                ]
    }

    function test_detailsNavigationMode(data) {
        compare(RoutePolicy.detailsNavigationMode(data.route, data.currentArgs, data.nextArgs, data.source), data.mode)
    }

    function test_detailsContextResolvesModelAndItem() {
        const fallback = modelFor([
                                      {
                                          movieId: "fallback"
                                      }
                                  ])
        const model = modelFor([
                                   {
                                       movieId: "first"
                                   },
                                   {
                                       movieId: "selected"
                                   }
                               ])
        const context = RoutePolicy.detailsContext({
                                                       model: model,
                                                       itemId: "selected",
                                                       focusIndex: 0,
                                                       source: "search",
                                                       returnRoute: "search"
                                                   }, fallback)
        compare(context.model, model)
        compare(context.index, 1)
        compare(context.item.movieId, "selected")
        compare(context.source, "search")
        compare(context.returnRoute, "search")
    }
}
