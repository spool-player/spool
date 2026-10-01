// Runs the actual RouteStack.showRoute/handleLoaded JS with deterministic loader
// and clock doubles. This tests ordering/reentrancy, not Qt rendering timings.
var Loader = { Ready: 1, Loading: 2, Error: 3 };
var pages, route, activeRoute, pendingLoader, uiTransitionToken;
var elapsed, startedAt, sampleName, sampleCacheHit, instanceMs, activations, forcedLoads;
var loadMode;
var console = { info: function() {} };
var settleWatchdog = { restart: function() {} };
var InputLatency = {
    beginUiTransition: function(name, from, to, hit) {
        startedAt = elapsed;
        sampleName = name;
        sampleCacheHit = hit;
        return 1;
    },
    mark: function(token, stage) {
        if (stage === 'instance') instanceMs = elapsed - startedAt;
    }
};
function pageKey(value) { return value; }
function activatePending() { ++activations; pendingLoader = null; }
function require(condition, message) { if (!condition) throw new Error(message); }
function loaderFor(key, synchronous) {
    if (pages[key]) return pages[key];
    require(synchronous, 'foreground construction remains synchronous');
    elapsed += 17;
    var loader = { status: loadMode === 'ready' ? Loader.Ready : Loader.Loading,
                   item: loadMode === 'ready' ? {children: []} : null, pageCacheKey: key };
    pages[key] = loader;
    // Actual Loader.onLoaded may run inside setSource before loaderFor returns.
    if (loader.status === Loader.Ready) handleLoaded(loader);
    return loader;
}
function reset() {
    pages = {}; route = 'search'; activeRoute = 'home'; pendingLoader = null;
    elapsed = 0; startedAt = -1; sampleName = ''; sampleCacheHit = '';
    instanceMs = -1; activations = 0; forcedLoads = 0; loadMode = 'ready';
}
function runRouteTransitionTests() {
    reset();
    showRoute();
    require(sampleCacheHit === 'miss' && sampleName === 'route:search:cold', 'new page must be a cold miss');
    require(instanceMs === 17, 'synchronous construction must be inside timing');
    require(activations === 1, 'synchronous construction activates once');

    reset();
    pages.search = {status: Loader.Ready, item: {children: []}};
    showRoute();
    require(sampleCacheHit === 'hit' && instanceMs === 0 && activations === 1, 'resident hit measured separately');

    reset();
    var promoted = {status: Loader.Loading, item: null};
    Object.defineProperty(promoted, 'asynchronous', {set: function(value) {
        if (value === false) {
            ++forcedLoads; elapsed += 13;
            this.item = {children: []}; this.status = Loader.Ready;
            handleLoaded(this);
        }
    }});
    pages.search = promoted;
    showRoute();
    require(sampleCacheHit === 'promoted' && instanceMs === 13, 'promotion includes synchronous finish');
    require(activations === 1 && forcedLoads === 1, 'onLoaded promotion must not activate twice');

    reset(); loadMode = 'loading';
    showRoute();
    require(activations === 0 && pendingLoader === pages.search, 'unready page must wait');
    elapsed += 20; pages.search.item = {children: []}; pages.search.status = Loader.Ready;
    handleLoaded(pages.search);
    require(instanceMs === 37 && activations === 1, 'asynchronous completion keeps original timing');

    reset(); activeRoute = '';
    showRoute();
    require(sampleCacheHit === 'miss' && instanceMs === 17 && activations === 1, 'startup includes construction');
    return 5;
}
