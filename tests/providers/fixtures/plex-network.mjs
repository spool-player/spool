// Endpoint-only routing: the real bundled Plex provider owns all auth/session behavior.
// Every request goes through ScriptRuntime's native HTTP/origin/cancellation boundary.
import { createSource as plex } from 'qrc:/providers/spool.plex/logic/provider.mjs';

export function createSource(configuration, host) {
    const endpoint = configuration.fixtureEndpoint;
    function route(original) {
        const routed = Object.assign({}, original);
        routed.http = (url, options) => original.http(url.indexOf('https://plex.tv/') === 0
            ? endpoint + url.slice('https://plex.tv'.length) : url, options);
        return routed;
    }
    const source = plex(configuration, route(host));
    const result = {};
    for (const key of Object.keys(source))
        result[key] = (args, operationHost) => source[key](args, route(operationHost));
    return result;
}
