// Echoes the resolve result a test supplies, so the runtime's admission of
// provider media metadata is exercised at the real script boundary.
export function createSource() {
    return {
        resolve(args) {
            // Real provider helpers emit explicit undefined for unknown facts.
            if (args.literal)
                return { url: 'http://127.0.0.1/literal.mkv', container: 'mkv',
                    source: { bitrate: 1500000, width: 3840, height: 2160,
                        sizeBytes: undefined, runtimeTicks: undefined }, delivery: undefined };
            // Null mode keeps null distinct from undefined at admission.
            if (args.nullish)
                return { url: 'http://127.0.0.1/nullish.mkv', source: { bitrate: 5000, sizeBytes: null } };
            return args.result;
        }
    };
}
