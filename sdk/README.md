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
