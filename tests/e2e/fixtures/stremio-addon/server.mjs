import { readFileSync } from 'node:fs';
import { createServer } from 'node:http';

const [mode, mediaPath] = process.argv.slice(2);
const bytes = readFileSync(mediaPath);
const manifest = {
    id: 'org.spool.synthetic', name: 'Spool Fixture', version: '1.0.0',
    description: 'Synthetic video for provider compatibility tests',
    resources: ['catalog', 'meta', 'stream'], types: ['movie'],
    catalogs: [{ type: 'movie', id: 'synthetic', name: 'Synthetic Movies', extra: [{ name: 'search' }] }]
};
const film = { id: 'spool-synthetic-film', type: 'movie', name: 'Synthetic Test Film' };
const catalog = async ({ extra }) => ({ metas: !extra.search || film.name.toLowerCase().includes(extra.search.toLowerCase()) ? [film] : [] });
const meta = async ({ id }) => ({ meta: id === film.id ? film : null });
const stream = async ({ id }) => ({ streams: id === film.id ? [{ name: 'Synthetic HTTP', url: origin + '/fixture.mp4' }] : [] });
const handlers = { catalog, meta, stream };
let origin;

function missing(_request, response) {
    response.writeHead(404).end();
}

async function fakeRouter(request, response) {
    const path = request.url;
    const resource = /^\/(catalog|meta|stream)\/movie\/([^/]+?)(?:\/([^/]+))?\.json$/.exec(path);
    const body = path === '/manifest.json' ? manifest : resource
        ? await handlers[resource[1]]({ id: decodeURIComponent(resource[2]),
            extra: Object.fromEntries(new URLSearchParams(resource[3])) })
        : null;
    if (!body) return missing(request, response);
    response.writeHead(200, { 'Content-Type': 'application/json' }).end(JSON.stringify(body));
}

let route = fakeRouter;
if (mode === 'sdk') {
    const { addonBuilder, getRouter } = (await import('stremio-addon-sdk')).default;
    const addon = new addonBuilder(manifest);
    addon.defineCatalogHandler(catalog);
    addon.defineMetaHandler(meta);
    addon.defineStreamHandler(stream);
    const sdkRouter = getRouter(addon.getInterface());
    route = (request, response) => sdkRouter(request, response, () => missing(request, response));
} else if (mode !== 'fake') {
    throw new Error('Expected sdk or fake fixture mode');
}

const server = createServer((request, response) => {
    if (request.url !== '/fixture.mp4') {
        route(request, response);
        return;
    }
    const headers = { 'Content-Type': 'video/mp4', 'Accept-Ranges': 'bytes' };
    const range = /^bytes=(\d+)-(\d*)$/.exec(request.headers.range || '');
    const start = range ? Number(range[1]) : 0;
    const end = range && range[2] ? Math.min(Number(range[2]), bytes.length - 1) : bytes.length - 1;
    if (start > end || (request.headers.range && !range)) {
        response.writeHead(416, { 'Content-Range': `bytes */${bytes.length}` }).end();
        return;
    }
    if (range) headers['Content-Range'] = `bytes ${start}-${end}/${bytes.length}`;
    headers['Content-Length'] = end - start + 1;
    response.writeHead(range ? 206 : 200, headers).end(request.method === 'HEAD' ? undefined : bytes.subarray(start, end + 1));
});
server.listen(0, '127.0.0.1', () => {
    origin = `http://127.0.0.1:${server.address().port}`;
    console.log(origin);
});
