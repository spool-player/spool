export function createSource(configuration, sourceHost) {
    let calls = 0;
    return {
        http(args, host) { ++calls; return host.http(args.url, args.options || {}); },
        sourceHttp(args) { return sourceHost.http(args.url, args.options || {}); },
        discover(args, host) {
            return host.discover(args).then(replies => ({array: Array.isArray(replies), replies: replies.map(reply => ({address: reply.address, text: reply.text}))}));
        },
        probe(args, host) { return host.probeLocalHttp(args); },
        state() { return {calls, sourceProbe: typeof sourceHost.probeLocalHttp}; }
    };
}
