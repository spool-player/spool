// Echoes the resolve result a test supplies, so the runtime's admission of
// provider media metadata is exercised at the real script boundary.
export function createSource() {
    return { resolve(args) { return args.result; } };
}
