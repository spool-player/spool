export function createSource(configuration, sourceHost) {
    let calls = 0;
    return {
        http(args, host) { ++calls; return host.http(args.url, args.options || {}); },
        sourceHttp(args) { return sourceHost.http(args.url, args.options || {}); },
        probe(args, host) { return host.probeLocalHttp(args); },
        state() { return {calls, sourceProbe: typeof sourceHost.probeLocalHttp}; }
    };
}
