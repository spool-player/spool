// SPDX-License-Identifier: MPL-2.0
// Run current first-party providers through the unmodified API-0.2 host glue.
import * as jellyfin from 'qrc:/providers/spool.jellyfin/logic/provider.mjs';
import * as emby from 'qrc:/providers/spool.emby/logic/provider.mjs';
import * as plex from 'qrc:/providers/spool.plex/logic/provider.mjs';

function require(value, message) {
    if (!value)
        throw new Error('legacy provider contract: ' + message);
}

function exercise(glue, name, module) {
    const isPlex = name === 'plex';
    const base = 'https://' + name + '.fixture.invalid';
    const token = name + '-private-token';
    const device = { id: 'device', name: 'Contract', app: 'Spool', version: '999.0', platform: 'test', locale: 'en' };
    const row = { Id: 'film', Name: 'Example', Type: 'Episode', SeriesId: 'series',
        ImageTags: { Primary: 'own-poster' }, SeriesPrimaryImageTag: 'series-poster',
        ParentThumbItemId: 'season', ParentThumbImageTag: 'parent-thumb',
        ParentBackdropItemId: 'series', ParentBackdropImageTags: ['parent-backdrop'] };
    const plexRow = { ratingKey: 'film', title: 'Example', type: 'movie', thumb: '/own-poster', duration: 60000,
        Media: [{ id: 'chosen', bitrate: 5000, container: 'mp4', Part: [{ id: 'part',
            key: '/library/parts/part/file.mp4', Stream: [{ streamType: 1, codec: 'h264', height: 720 }] }] }] };
    const requests = [];
    const reports = [];
    const bridge = {
        http: function(url, options, ok, no) {
            try {
                require(url.indexOf(base + '/') === 0, name + ': credentials stay on the account origin');
                const authenticated = name === 'jellyfin'
                    ? options.headers.Authorization.indexOf('Token="' + token + '"') >= 0
                    : options.headers[isPlex ? 'X-Plex-Token' : 'X-Emby-Token'] === token;
                require(authenticated, name + ': account credentials survive legacy HTTP glue');
                const path = url.slice(base.length).split('?')[0].replace(/^\/emby\//, '/');
                const method = options.method || 'GET';
                requests.push(method + ' ' + path);
                let result;
                if (path === '/Sessions/Capabilities/Full')
                    result = {};
                else if (path === '/Items' || path === '/Users/user/Items')
                    result = { Items: [row], TotalRecordCount: 1 };
                else if (path === '/Users/user/Items/film')
                    result = row;
                else if (path === '/MediaSegments/film')
                    result = { Items: [] };
                else if (path === '/Items/film/PlaybackInfo') {
                    const body = JSON.parse(options.body);
                    require(body.MediaSourceId === 'chosen', name + ': selected edition reaches playback negotiation');
                    result = { PlaySessionId: 'session', MediaSources: [{ Id: 'chosen', SupportsDirectPlay: true,
                        Bitrate: 5000000, Container: 'mp4' }] };
                } else if (path === '/library/sections/1/all')
                    result = { MediaContainer: { Metadata: [plexRow], totalSize: 1 } };
                else if (path === '/library/metadata/film')
                    result = { MediaContainer: { Metadata: [plexRow] } };
                else if (path === '/Sessions/Playing' || path === '/Sessions/Playing/Progress'
                    || path === '/Sessions/Playing/Stopped') {
                    require(method === 'POST', name + ': reports use POST');
                    reports.push(JSON.parse(options.body));
                    result = {};
                } else if (path === '/:/timeline') {
                    const values = {};
                    for (const part of url.split('?')[1].split('&')) {
                        const pair = part.split('=');
                        values[decodeURIComponent(pair[0])] = decodeURIComponent(pair[1]);
                    }
                    reports.push(values);
                    result = { MediaContainer: {} };
                } else
                    throw new Error(name + ': unexpected request ' + method + ' ' + path);
                ok({ status: 200, body: JSON.stringify(result) });
            } catch (error) { no(error); }
        },
        // Hold source-owned timers/sockets idle; the contract exercises only explicit operations.
        delay: function() {},
        socket: function() { return 'socket'; },
        socketSend: function() {},
        socketClose: function() {},
        emitEvent: function() {},
        speedTest: function() { throw new Error(name + ': unsupported probe reached native bridge'); }
    };
    const source = glue.create({ createSource: function(configuration, host) {
        require(!('extensions' in host), name + ': frozen baseline source host has no extensions');
        return module.createSource(configuration, host);
    } }, { server: base, userId: 'user', token: token, serverId: 'machine' }, bridge, device);
    function call(method, args) {
        return new Promise(function(resolve, reject) {
            const operation = Object.assign({}, bridge, { resolve: resolve, reject: reject });
            glue.call(source, method, args || {}, operation, bridge, device);
        });
    }
    let playback;
    return call('describe').then(description => {
        require(Object.keys(description.extensions).length === 0 && description.artwork.indexOf(base) === 0,
            name + ': baseline source exposes artwork but offers no extensions');
        return call('extensionStatus');
    }).then(status => {
        require(Object.keys(status.enabled).length === 0 && status.missingHost.indexOf('spool.speed-test') >= 0,
            name + ': compatibility notice is discoverable through a baseline operation');
        const before = requests.length;
        return call('speedTest').then(() => { throw new Error(name + ': unsupported speed test succeeded'); }, error => {
            require(error.message === 'unsupported_extension' && requests.length === before,
                name + ': unsupported extension is rejected before HTTP/native probes');
        });
    }).then(() => call('browse', { parentId: isPlex ? 'section:1' : 'library', limit: 10 }))
        .then(page => {
            require(page.items.length === 1 && page.items[0].id === 'film' && page.items[0].title === 'Example'
                && page.exhausted && page.cursor === null, name + ': baseline browse remains usable');
            if (!isPlex) {
                const item = page.items[0];
                require(!item.thumbTag && !item.backdropTag && !item.thumbItemId && !item.backdropItemId
                    && item.posterTag === 'own-poster' && item.seriesPosterTag === 'series-poster',
                    name + ': old artwork contract keeps safe own/series images without inherited child URLs');
            }
            return call('resolve', { itemId: 'film', variantId: 'chosen', positionTicks: '0' });
        }).then(result => {
            playback = result;
            require(result.variantId === 'chosen' && result.playMethod === 'DirectPlay' && result.url.indexOf(base) === 0
                && result.headers[isPlex ? 'X-Plex-Token' : 'X-Emby-Token'] === token,
                name + ': baseline resolve retains edition and authenticated direct playback');
            let sequence = Promise.resolve();
            for (const event of ['start', 'progress', 'stop'])
                sequence = sequence.then(() => call('report', { event: event, itemId: 'film', variantId: 'chosen',
                    playSessionId: playback.playSessionId, playMethod: playback.playMethod,
                    positionTicks: '123450000', subtitleStreamIndex: -1 }));
            return sequence;
        }).then(() => {
            require(reports.length === 3, name + ': start, progress and stop reach the server');
            if (isPlex)
                require(reports.map(report => report.state).join(',') === 'playing,playing,stopped'
                    && reports.every(report => report.ratingKey === 'film' && report.time === '12345'),
                    'plex: legacy reporting preserves item, state and position');
            else
                require(reports.every(report => report.ItemId === 'film' && report.MediaSourceId === 'chosen'
                    && report.PositionTicks === 123450000 && report.SubtitleStreamIndex === -1),
                    name + ': legacy reporting preserves item, edition, position and subtitle off');
        });
}

export function run(glue) {
    return exercise(glue, 'jellyfin', jellyfin).then(() => exercise(glue, 'emby', emby))
        .then(() => exercise(glue, 'plex', plex));
}
