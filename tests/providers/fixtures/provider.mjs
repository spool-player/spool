export function createSource(config, sourceHost) {
    let calls = 0;
    const item = function(id) { return {id: id, title: config.label + ' ' + id, type: 'Movie'}; };
    const failing = function() { if (config.failing) throw new Error('offline'); };
    return {
        describe: function() { return {artwork: 'https://img.invalid/{itemId}/{type}?w={width}'}; },
        libraries: function() {
            failing();
            return {items: (config.libraries || ['lib']).map(function(id) {
                return {id: id, title: id === 'lib' ? 'Shelf' : id, collectionType: 'movies'};
            })};
        },
        // One film per library the account sees, and one exact title on request.
        search: function(args) {
            failing();
            const rows = (config.libraries || ['lib']).map(function(id) {
                return {id: id + '-1', title: 'Film in ' + id, type: 'Movie'};
            });
            if (config.exact)
                rows.push({id: 'exact', title: 'The Film', type: 'Movie'});
            return {items: rows, cursor: null, exhausted: true};
        },
        latest: function(args) {
            failing();
            return {items: [item('new-1'), item('new-2'), item('new-3')].slice(0, args.limit), cursor: null,
                exhausted: true};
        },
        details: function(args) { return {item: item(args.itemId)}; },
        items: function(args) { failing(); return {items: args.ids.map(item), cursor: null, exhausted: true}; },
        runItemAction: function(args) {
            if (!args.name) return {pick: {kind: 'name'}};
            return {changed: true, itemId: args.itemId, message: args.action + ':' + args.name};
        },
        announce: function(args) { sourceHost.emit('changed', {itemId: args.itemId}); return {}; },
        configuration: function() { return {configuration: config}; },
        expired: function() { throw new Error('http_401'); },
        bump: function() { return {calls: ++calls, label: config.label}; },
        candidates: function(args) {
            const rows = [];
            for (let i = 0; i < args.count; ++i)
                rows.push({id: String(i), title: config.label + ' ' + i, variantId: 'file-' + i});
            return {items: rows, exhausted: true};
        },
        mediaPage: function(args) {
            const rows = [];
            for (let i = 0; i < args.count; ++i)
                rows.push({id: String(i), sourceId: 'spoofed', title: config.label + ' ' + i,
                    type: 'Movie', year: 2020, externalIds: {Tmdb: String(i + 1)},
                    runtimeTicks: '12345678900'});
            return {items: rows, cursor: null, total: args.count, exhausted: true};
        },
        delayedMediaPage: function(args, host) {
            return host.delay(10000).then(function() { return {items: [], exhausted: true}; });
        },
        raw: function(args, host) {
            return host.http(config.origin + '/' + args.path);
        },
        speedTest: function(args, host) {
            return host.speedTest({
                url: args.url || config.origin + '/' + (args.path || 'speed') + '?bytes={bytes}&nonce={nonce}',
                headers: args.headers || {Authorization: config.token}
            });
        },
        speedHosts: function() { return {sourceCanMeasure: typeof sourceHost.speedTest === 'function'}; },
        overlappingSpeedTests: function(args, host) {
            const options = {url: config.origin + '/speed?bytes={bytes}&nonce={nonce}'};
            const first = host.speedTest(options);
            return host.speedTest(options).then(function() {
                throw new Error('overlap_accepted');
            }, function(error) {
                return first.then(function(result) { return {error: String(error), bitrate: result.bitrate}; });
            });
        },
        delay: function(args, host) {
            return host.delay(args.milliseconds).then(function() { return {completed: true}; });
        },
        timerSpin: function(args, host) {
            return host.delay(10).then(function() { while (true) {} });
        },
        page: function(args, host) {
            return host.http(config.origin + '/items', {
                headers: {Authorization: config.token}
            }).then(function(response) {
                if (response.status !== 200)
                    throw new Error('HTTP failure');
                const body = JSON.parse(response.body);
                return {
                    items: body.items.map(function(item) {
                        return {id: item.id, title: config.label + ': ' + item.name};
                    }),
                    cursor: body.cursor,
                    calls: ++calls
                };
            });
        },
        denied: function(args, host) {
            return host.http(args.url).then(function() { return {}; });
        },
        throws: function() { throw new Error('private-token-never-log'); },
        cycle: function() { const value = {}; value.self = value; return value; },
        largeInteger: function() { return {size: 9007199254740992}; },
        never: function() { return new Promise(function() {}); },
        spin: function() { while (true) {} },
        asyncSpin: function() { return Promise.resolve().then(function() { while (true) {} }); },
        state: function() { return {calls: calls}; }
    };
}
