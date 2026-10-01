export function createSource(config, sourceHost) {
    let calls = 0;
    const batches = [];
    let activeBatches = 0;
    let maximumBatches = 0;
    let queueCalls = 0;
    let queue = [];
    const reports = [];
    const item = function(id) {
        const row = {id: id, title: config.label + ' ' + id, type: 'Movie'};
        if (config.pagination)
            row.entryId = 'entry:' + id;
        if (config.inheritedArtwork) {
            row.thumbTag = 'thumb-tag';
            row.thumbItemId = 'parent-thumb';
            row.backdropTag = 'backdrop-tag';
            row.backdropItemId = 'parent-backdrop';
        }
        return row;
    };
    const failing = function() { if (config.failing) throw new Error('offline'); };
    const paginated = function(args) {
        const kind = args.seriesId || args.personId || args.parentId;
        const index = args.cursor === undefined ? 0 : Number(args.cursor.slice(2));
        if (args.cursor !== undefined && args.cursor !== 's:' + index)
            throw new Error('invalid_pagination');
        if (kind === 'missing')
            return {items: [], exhausted: false};
        if (kind === 'repeat')
            return {items: [], cursor: 's:1', exhausted: false};
        if (kind === 'empty-forever')
            return {items: [], cursor: 's:' + (index + 1), exhausted: false};
        if (kind === 'sparse') {
            if (index === 0)
                return {items: [item('first'), item('second')], cursor: 's:1', exhausted: false};
            if (index === 1)
                return {items: [], cursor: 's:2', exhausted: false};
            return {items: [item('last')], cursor: null, exhausted: true};
        }
        const total = kind === 'too-many' ? 10001 : 205;
        const end = Math.min(index + args.limit, total);
        const rows = [];
        for (let i = index; i < end; ++i)
            rows.push(item(String(i)));
        return {items: rows, cursor: end < total ? 's:' + end : null, exhausted: end >= total};
    };
    return {
        describe: function() {
            return {artwork: 'https://img.invalid/{itemId}/{type}?w={width}',
                extensions: config.catalogueExtensions
                    ? {'spool.suggestions': 1, 'spool.playback-queue-reporting': 1} : {}};
        },
        suggestions: function(args) {
            return {items: [item('suggestion')].slice(0, args.limit), cursor: null, exhausted: true};
        },
        report: function(args) {
            if (args.queue && !Array.isArray(args.queue.items))
                throw new Error('invalid_queue');
            reports.push(args);
            return {};
        },
        reportStats: function() { return {reports: reports}; },
        queueStatus: function(args) { sourceHost.emit('playbackQueueStatus', args); return {}; },
        libraries: function() {
            failing();
            return {items: (config.libraries || ['lib']).map(function(id) {
                return {id: id, title: id === 'lib' ? 'Shelf' : id, collectionType: 'movies'};
            })};
        },
        // One film per library the account sees, and one exact title on request.
        search: function(args) {
            failing();
            if (config.searchItems)
                return {items: config.searchItems.slice(0, args.limit), cursor: null, exhausted: true};
            const rows = (config.libraries || ['lib']).map(function(id) {
                return {id: id + '-1', title: 'Film in ' + id, type: 'Movie', year: 2020};
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
        browse: paginated,
        seasons: paginated,
        episodes: paginated,
        personItems: paginated,
        resume: paginated,
        nextUp: paginated,
        batchStats: function() {
            return {requests: batches, maximumActive: maximumBatches, queueCalls: queueCalls, queue: queue};
        },
        groupSend: function(args) {
            ++queueCalls;
            queue = Array.from(args.itemIds);
            return {};
        },
        details: function(args) { return {item: item(args.itemId)}; },
        items: function(args, host) {
            failing();
            if (!config.pagination)
                return {items: args.ids.map(item), cursor: null, exhausted: true};
            batches.push(Array.from(args.ids));
            ++activeBatches;
            maximumBatches = Math.max(maximumBatches, activeBatches);
            return host.delay(5).then(function() {
                --activeBatches;
                return {items: args.ids.filter(function(id) { return id !== 'missing'; }).reverse().map(item),
                    cursor: null, exhausted: true};
            });
        },
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
                range: args.range || false,
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
