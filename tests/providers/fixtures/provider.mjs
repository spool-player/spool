export function createSource(config) {
    let calls = 0;
    return {
        raw: function(args, host) {
            return host.http(config.origin + '/' + args.path);
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
