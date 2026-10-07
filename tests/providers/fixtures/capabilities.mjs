export function createSource(configuration, sourceHost) {
    const initial = sourceHost.capabilities;
    if (configuration.event)
        sourceHost.emit('capabilitiesChanged', {capabilities: configuration.event});
    let previous;
    return {
        inspect(args, host) {
            let blocked = 0;
            for (const map of [initial, host.capabilities]) {
                try { map.speedTest = false; } catch (_) { ++blocked; }
                try { map.suggestions = true; } catch (_) { ++blocked; }
                try { delete map.speedTest; } catch (_) { ++blocked; }
            }
            try { host.capabilities = {}; } catch (_) { ++blocked; }
            try { sourceHost.capabilities = {}; } catch (_) { ++blocked; }
            const result = {
                capabilities: host.capabilities,
                shared: initial === host.capabilities && (!previous || previous === host.capabilities),
                frozen: Object.isFrozen(initial) && Object.isFrozen(host.capabilities),
                blocked: blocked
            };
            previous = host.capabilities;
            return result;
        }
    };
}
