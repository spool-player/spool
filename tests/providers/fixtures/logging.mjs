export function createSource(config, sourceHost) {
    let lazy = 0, reads = 0;
    sourceHost.log('info', function() { ++lazy; return 'source-created ' + config.token; });
    return {
        describe: function() { return {}; },
        exercise: function(args, host) {
            const enabled = {};
            const fields = { count: 3, status: 200, note: config.token, nested: {secret: 'not-serialized'} };
            Object.defineProperty(fields, 'expensive', {
                enumerable: true,
                get: function() { ++reads; return 42; }
            });
            Object.defineProperty(fields, 'Authorization', {
                enumerable: true,
                get: function() { throw new Error('credential-getter-must-not-run'); }
            });
            ['trace', 'debug', 'info', 'warn', 'error'].forEach(function(level) {
                enabled[level] = host.isLogEnabled(level);
                host.log(level, function() { ++lazy; return 'operation-' + level + ' ' + config.token; }, fields);
            });
            // No coercion or serialization of objects is part of the log API.
            host.log('info', {toString: function() { throw new Error('no-coercion'); }}, fields);
            sourceHost.log('debug', function() { ++lazy; return 'retained-source-host'; });
            host.log('unknown', function() { throw new Error('invalid-level'); });
            return {enabled: enabled, lazy: lazy, reads: reads, invalid: host.isLogEnabled('unknown')};
        },
        lateCredentials: function(args, host) {
            // These credentials are acquired after source creation, so none
            // can be redacted through the source configuration secret list.
            const samples = [
                'Cookie: session=late-cookie-value; renewal=late-renewal-value\nstatus=200',
                'Set-Cookie: session=late-setcookie-value; Expires=Wed, 09 Jun 2027 10:18:14 GMT; HttpOnly\nstatus=201',
                '{"Authorization":"Bearer late-auth-value","count":7}',
                "{'Authorization':'Basic late-basic-value','count':8}",
                '{"Cookie":"session=late-jsoncookie-value; renewal=late-jsonrenewal-value","count":9}',
                '{"Set-Cookie":"session=late-jsonsetcookie-value; HttpOnly","count":10}',
                '{"Proxy-Authorization":"Bearer late-proxy-value","count":11}'
            ];
            samples.forEach(function(sample) {
                host.log('info', sample, {status: 200, note: sample});
            });
            return {count: samples.length};
        },
        safety: function(args, host) {
            host.log('info', 'credentials ' + config.token + ' ' + config.refreshToken +
                ' password="two word password" URL=https://private.example/stream?token=url-secret\n' +
                'socket=wss://private.example/socket?token=socket-secret ' +
                'magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567');
            host.log('info', 'fields', {refreshToken: 'field-secret', userName: 'Alice',
                streamUrl: 'https://private.example/signed', infoHash: 'field-hash',
                note: config.refreshToken, count: 12, success: true});
            host.log('info', 'x'.repeat(2049) + config.token);
            host.log('info', 'bounded-fields', {note: 'x'.repeat(257) + config.token});
            const many = {};
            for (let i = 0; i < 100; ++i) many['field_' + i] = 'x'.repeat(200);
            host.log('info', 'bounded-total', many);
            return {};
        }
    };
}
