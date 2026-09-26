# Spool provider SDK (API 0.2)

A provider teaches Spool a media source: a server, a service, a folder. It is a
small package of JavaScript (the logic) and optional QML (its own sign-in,
settings and picker screens). Spool runs every provider the same way, whether
bundled with the app, installed from the store or added from a link.

| File | What it is |
| --- | --- |
| `provider.d.ts` | The contract: `createSource`, every operation, events and the screen context |
| `spool-provider.py` | Builds, validates and describes packages (`build`, `validate`, `feed`) |
| `provider-contract-runner.cpp` | Runs a provider's `tests/contract.mjs` in Qt's JS engine, as Spool does |

`spool-player/spool-provider-example` is a complete provider to start from.

## Packages

```
manifest.json      format 2 (below)
LICENSE, NOTICE
logic/*.mjs        entry module exporting createSource(configuration, host)
ui/*.qml           optional screens named in manifest.ui
assets/            icon and anything else the screens show
```

```json
{
  "format": 2, "api": "0.2",
  "id": "publisher.name", "name": "Shown name", "version": "1.2.3",
  "summary": "One line, up to 120 characters", "publisher": "You", "homepage": "https://…",
  "icon": "assets/icon.svg", "entry": "logic/provider.mjs",
  "capabilities": ["search", "userState", "reporting", "segments", "streamQuality", "trickplay",
                   "discovery", "groupPlayback", "remoteControl", "speedTest"],
  "origins": ["https://api.example.org"],
  "ui": { "login": "ui/Login.qml", "settings": "ui/Settings.qml", "picker": "ui/Picker.qml" },
  "actions": [{ "id": "playlist", "label": "Add to playlist", "icon": "playlist_add", "types": ["Movie"] }]
}
```

- A provider with a `login` screen needs an account; one without is added straight away.
- `origins` are reachable by every account; `*` allows any HTTP(S) origin. Anything else an account
  reaches is what its login screen allowed with `provider.allowOrigin(url)`.
- `actions` appear in the item menu for the listed types and run through `runItemAction`.
- Packages are `.tar.zst` (ustar, zstd), at most 16 MiB, 512 files, 32 MiB expanded. Paths are
  relative, without hidden parts, of the listed types; links and native binaries are refused.

```
python3 sdk/spool-provider.py build path/to/provider           # dist/<id>-<version>.tar.zst
python3 sdk/spool-provider.py validate dist/<id>-<version>.tar.zst
python3 sdk/spool-provider.py feed dist/<id>-<version>.tar.zst --url https://…/<id>-<version>.tar.zst
```

Building is reproducible. It needs Python 3.14, or the `zstd` command on older Pythons.

## Running

Each provider module gets one worker thread and QJSEngine; `createSource` is called once per account
with that account's configuration and a host that lives as long as the account. Keep account state in
that closure. Operations are called as `operation(args, host)` and return a plain value or a Promise.
Throw `new Error('snake_case_code')` to fail: the code reaches Spool (`http_401` asks the viewer to sign
in again), anything else becomes `provider_error`. ES2020 modules and Promises only: no `async`/`await`,
no Node or browser globals, and Qt's engine lacks some newer built-ins such as `Array.prototype.flatMap`.

| Limit | |
| --- | --- |
| Uninterrupted script | 500 ms; exceeding it turns the module off until restarted |
| Operation | settles within 15 s; eight in flight per account |
| HTTP | four at once per operation, 1 MiB bodies, 8 MiB responses, redirects returned not followed, no cookies |
| Sockets | `host.socket` on the source host, four per account |
| Timers | `host.delay`: 0–60 s on the source host, 0–10 s in an operation, 16 pending |
| Results | 50,000 values, depth 20, arrays of 10,000, 4 MiB of text; ticks as decimal strings |

This is a reviewed, in-process profile, not a sandbox: install providers you trust.

## Connection speed

Declare `speedTest` when the service offers a bounded download endpoint, then
implement the operation using the native operation host:

```js
speedTest(args, host) {
    return host.speedTest({
        url: server + "/download-test?bytes={bytes}&nonce={nonce}",
        headers: { Authorization: authorization() }
    });
}
```

Spool substitutes `{bytes}` and a unique `{nonce}` for each request. Return
exactly that many uncompressed bytes with HTTP 200. The URL must stay on an
allowed HTTP(S) origin; redirects, cookies, truncated and oversized samples
are rejected. HTTP errors remain `http_NNN`, including `http_401`.

For servers without a generated test endpoint, pass
`{url: mediaUrl, headers: authorizationHeaders, range: true}` for an accessible
static media file at least 4 MiB long. Spool supplies bounded `Range` headers,
requires HTTP 206 and an exact `Content-Range` with a valid total size, and
rejects servers that ignore ranges. URL placeholders are optional in this mode.
Each parallel round divides the first 4 MiB into disjoint ranges; cache-control
requests bypass HTTP caches. The measurement includes media-server storage
and transport overhead, without starting playback or a transcoding session.
Providers cannot supply their own `Range` or compression headers.

The worker warms 512 KiB, measures one/two connections with 4 MiB totals, and
tries four if warmup time-to-first-byte is at least 20 ms or two improve the
rate by at least 10%. It chooses the fewest lanes within 85% of the fastest,
returns that lane count's rate with 25% headroom, and clamps to 1–1000 Mbps.
The result is `{bitrate, parallelRequests}`. Bodies never reach JS; the probe
reserves the operation's HTTP slots and shares its 15-second deadline and
cancellation. `speedTest` exists only on the operation host, not the source host.

Spool schedules probes while idle and passes each account's result back in
`PlaybackContext.measuredBitrate` (zero before measurement) and
`parallelRequests` (two before measurement). Use the measured ceiling only
when the viewer has not chosen a session or settings limit. Spool shows the
result under Quality → Auto and in Streaming settings.

## Quality policy

Quality limits are backend-neutral ceilings in bits/second and pixels, not
transcoder presets. Providers translate them into their service's negotiation
or source selection. Keep the same precedence across providers:

1. A nonzero player `maxBitrate` overrides automatic bitrate selection.
2. `unlimitedLocalNetwork` applies only when the media server positively
   identifies this connection as local; a failed lookup never implies local.
3. Otherwise use `preferredMaxBitrate`, then `measuredBitrate`, then a
   provider-documented fallback. An explicit choice may exceed the measurement.
4. Independently use `maxHeight`, then `preferredMaxHeight`; zero means no
   height ceiling. A local-network bitrate exemption does not remove it.

For Jellyfin and Emby, send these limits in PlaybackInfo and DeviceProfile;
for Plex, translate bits/second to the server's kbit/second bandwidth setting
and negotiate whether the selected media can direct play, remux or transcode.
Never treat a remux preference as permission to exceed a quality ceiling.
Preserve an explicitly selected edition rather than silently substituting one.

A source-only service need not expose a transcoder. A future Stremio-style
provider can use the same context to select among known stream variants and
return `pick` for its provider-owned QML picker when a choice is needed.
[Stremio's stream contract](https://github.com/Stremio/stremio-addon-sdk/blob/master/docs/api/responses/stream.md)
offers descriptive names and optional `videoSize`, not a standardized numeric
bitrate, height or transcoding API. Do not infer reliable constraints from a
quality label alone; size and duration give average bitrate, not peak demand.
Unknown variants should remain visibly unknown in the picker rather than
being presented as satisfying a limit.

Only measure an endpoint on the actual media route. A generic Internet speed
test, an add-on catalogue host, or a local torrent gateway does not establish
throughput from the stream's CDN or peers. If the service has no suitable
bounded endpoint, omit `speedTest`; retain explicit quality/source choices
instead of inventing a measurement. Per-account measurements are appropriate
for a fixed media server, not interchangeable across arbitrary stream origins.

## Screens

A screen is mounted with a `provider` property (`ScreenContext` in `provider.d.ts`) and may
`import QtQuick`, `QtQuick.Layouts`, `QtQuick.Controls`, `QtQml`, `QtQml.Models` and `Spool` (the
app's theme, metrics, input keys and primitives). `request()` calls an operation of this account;
`requestList()` streams `items` into the native `rows` model; `complete()` or `close()` settles the
screen once. Map error codes to your own words.

## Testing

```
cmake -S sdk -B build/sdk && cmake --build build/sdk
build/sdk/provider-contract-runner tests/contract.mjs
QV4_FORCE_INTERPRETER=1 build/sdk/provider-contract-runner tests/contract.mjs
```

`tests/contract.mjs` exports `run()`, which returns a Promise or throws; the runner prints why a
contract failed and gives up after 10 seconds.

## Publishing

Attach the package and its `spool-provider.json` (the `feed` output) to each release. Spool can then
install it from a link to the repository: GitHub resolves to
`releases/latest/download/spool-provider.json`, GitLab to
`-/releases/permalink/latest/downloads/spool-provider.json`, and any other site to
`/spool-provider.json` at the address given. Installed providers are updated from the same place.

To be listed in the store, open a pull request on `spool-player/spool-providers` adding
`providers/<id>.json` with that feed entry. CI downloads the package, checks its digest and validates
it; once merged, the store site is rebuilt and the provider appears in Spool.
