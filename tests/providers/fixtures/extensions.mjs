export function createSource(configuration, sourceHost) {
    const initial = sourceHost.extensions;
    if (configuration.event)
        sourceHost.emit('extensionsChanged', {extensions: configuration.event});
    let previous;
    return {
        inspect(args, host) {
            let blocked = 0;
            for (const map of [initial, host.extensions]) {
                try { map['spool.speed-test'] = 99; } catch (_) { ++blocked; }
                try { map['spool.suggestions'] = 1; } catch (_) { ++blocked; }
                try { delete map['spool.speed-test']; } catch (_) { ++blocked; }
            }
            try { host.extensions = {}; } catch (_) { ++blocked; }
            try { sourceHost.extensions = {}; } catch (_) { ++blocked; }
            const result = {
                extensions: host.extensions,
                shared: initial === host.extensions && (!previous || previous === host.extensions),
                frozen: Object.isFrozen(initial) && Object.isFrozen(host.extensions),
                blocked: blocked
            };
            previous = host.extensions;
            return result;
        }
    };
}
