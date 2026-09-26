// The Jellyfin provider as bundled (qrc), against a scripted server: the
// provider's own repository carries the full contract; this proves the pinned
// package loads in this Qt and speaks API 0.2.
import {createSource} from 'qrc:/providers/spool.jellyfin/logic/provider.mjs';

function require(value, message) {
    if (!value) throw new Error('bundled provider contract failed: ' + message);
}

function respond(value) {
    return Promise.resolve({status: 200, body: JSON.stringify(value)});
}

export function run() {
    const device = {id: 'device', name: 'Test', app: 'Spool', version: '0', platform: 'test', locale: 'en'};
    const source = createSource({server: 'https://fixture.invalid', userId: 'user', token: 'fixture-token'},
        {device: device});
    const host = {device: device, http: function(url, options) {
        require(options.headers.Authorization.indexOf('Token="fixture-token"') >= 0, 'token in every request');
        if (url.indexOf('/PlaybackInfo') >= 0) {
            require(JSON.parse(options.body).MediaSourceId === 'chosen', 'the chosen edition is requested');
            return respond({PlaySessionId: 'session', MediaSources: [
                {Id: 'other', SupportsDirectPlay: true},
                {Id: 'chosen', SupportsDirectPlay: true, Container: 'mkv,webm'}
            ]});
        }
        if (url.indexOf('/MediaSegments/') >= 0)
            return respond({Items: [{Type: 'Intro', StartTicks: 0, EndTicks: 300000000}]});
        if (url.indexOf('/Users/user/Items/film') >= 0)
            return respond({Id: 'film'});
        require(url.indexOf('StartIndex=1') >= 0 && url.indexOf('Limit=1') >= 0, 'cursor and limit are sent');
        return respond({TotalRecordCount: 2, Items: [
            {Id: 'film', Name: 'Example', Type: 'Movie', ProductionYear: 2020, ProviderIds: {Imdb: 'tt1'}}
        ]});
    }};
    require(source.describe().artwork.indexOf('https://fixture.invalid/Items/{itemId}/Images/{type}') === 0,
        'artwork is a URL template');
    return source.search({query: 'Example', cursor: '1', limit: 1}, host).then(function(page) {
        require(page.exhausted && page.cursor === null && page.total === 2, 'paging');
        require(page.items[0].id === 'film' && page.items[0].externalIds.Imdb === 'tt1', 'items');
        return source.resolve({itemId: 'film', variantId: 'chosen', positionTicks: '0'}, host);
    }).then(function(playback) {
        require(playback.variantId === 'chosen' && playback.playMethod === 'DirectPlay', 'edition');
        require(playback.url.indexOf('MediaSourceId=chosen') >= 0, 'stream URL');
        require(playback.headers['X-Emby-Token'] === 'fixture-token', 'stream credentials');
        require(playback.container === 'mkv' && playback.segments[0].type === 'Intro', 'details');
        return source.search({query: 'x'}, {device: device, http: function() {
            return Promise.resolve({status: 401, body: ''});
        }}).then(function() { require(false, '401 rejects'); }, function(error) {
            require(error.message === 'http_401', 'a rejected token is reported as http_401');
        });
    });
}
