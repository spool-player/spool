import {createSource} from 'qrc:/providers/spool.jellyfin/logic/provider.mjs';

function require(value) {
    if (!value) throw new Error('bundled provider contract failed');
}

export function run() {
    const source = createSource({server: 'https://fixture.invalid', userId: 'user', token: 'fixture-token'});
    const host = {http: function(url, options) {
        require(options.headers.Authorization.indexOf('fixture-token') >= 0);
        if (url.indexOf('/PlaybackInfo') >= 0) {
            require(JSON.parse(options.body).MediaSourceId === 'chosen');
            return Promise.resolve({status: 200, body: JSON.stringify({PlaySessionId: 'session', MediaSources: [
                {Id: 'other', SupportsDirectPlay: true},
                {Id: 'chosen', SupportsDirectPlay: true}
            ]})});
        }
        require(url.indexOf('StartIndex=1') >= 0 && url.indexOf('Limit=1') >= 0);
        return Promise.resolve({status: 200, body: JSON.stringify({TotalRecordCount: 2, Items: [
            {Id: 'film', Name: 'Example', Type: 'Movie', ProductionYear: 2020, ProviderIds: {Imdb: 'tt1'}}
        ]})});
    }};
    return source.search({query: 'Example', cursor: '1', limit: 1}, host).then(function(page) {
        require(page.exhausted && page.cursor === null);
        require(page.items[0].id === 'film' && page.items[0].externalIds.Imdb === 'tt1');
        return source.resolve({itemId: 'film', variantId: 'chosen'}, host);
    }).then(function(playback) {
        require(playback.variantId === 'chosen');
        require(playback.video.url.indexOf('MediaSourceId=chosen') >= 0);
        require(playback.video.headers['X-Emby-Token'] === 'fixture-token');
    });
}
