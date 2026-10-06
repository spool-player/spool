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
| Uninterrupted script | 500 ms (worker CPU time on Windows, excluding loader I/O and descheduling); exceeding it turns the module off until restarted |
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
An in-flight bounded probe finishes even if playback starts. New automatic
probes wait for idle; an explicit refresh may measure during playback.


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
`Resolved.source` can supply fresh selected-edition bitrate and pixel dimensions
from playback negotiation. Quality menus use this original analysis rather than
a stale catalogue summary, a measured automatic limit, or transcoded output.


`Resolved.timelineOriginTicks` identifies the source position represented by
normalized media time zero (zero when omitted). A server-started HLS stream
that already begins at the resume point must advertise that origin, so Spool
seeks only the remaining fraction instead of seeking the full resume offset
again. UI positions, reports, chapters and segment boundaries remain absolute
source positions. Seeking before the stream origin resolves a fresh stream.
Direct files and full-timeline streams keep origin zero.

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

## Offline downloads

Declare `downloads` for original media and additionally `downloadTranscode` only
when the server can produce a finite, complete encoded media file. Implement
`download({itemId, mode, maxBitrate?, maxHeight?, variantId?}, host)` and return
`{url, container, headers?, size?, cleanup?}`. `mode` is `original` or `transcoded`;
encoding is performed by the server, never on the viewer's device. `container`
is a local-playable media suffix, such as `mp4`, `mkv`, or `flac`.

The endpoint must finish at EOF and contain the whole item from its beginning.
An HLS/DASH manifest, live resource, or saved online playlist is not a download.
Progressive finite server endpoints are supported, including responses without
a known length. The host streams bounded chunks to a temporary file and publishes
the copy only after success. URLs must be approved account origins; native requests
reuse account TLS trust, reject redirects, omit cookies, and use only the returned
headers. Do not put credentials in filenames or metadata.

A provider-owned release/file picker can return `PickRequest`; the host repeats
`download` with the submitted choice merged in, preserving the selected item,
mode and quality ceilings. A `cleanup` object is opaque and lives only in memory.
Implement `downloadRelease({cleanup}, host)` when server-session cleanup is needed;
it is called on completion, cancellation and failure. Interrupted downloads are
shown as retryable failures after restart, not silently resumed with stale URLs.

## Artwork ownership

An image tag belongs to an item, not necessarily the row that displays it.
When a thumbnail or backdrop is inherited, return `thumbItemId` or
`backdropItemId` alongside its tag. Omit the owner for the row's own image.
Spool scopes these opaque IDs to the account and preserves them in cached
media rows; home cards and details request the image from that owner.
Do not attach a parent's tag to a child without its owner ID. Series posters
and album covers retain their existing `seriesId`/`albumId` ownership.

## Seek previews

Return `resolve().trickplay` for the selected media variant, not a global URL
template in `describe()`. Sprite sheets use
`{width, height, columns, rows, count, intervalMs, urlTemplate}`; the absolute
HTTP(S) template has one `{index}` substitution. BIF sequences use
`{format: "bif", url}` with optional `width`/`height`. Pass the whole sequence
URL, not individual JPEGs: C++ parses its timestamp/offset index and decodes
the selected frame. Missing server-generated previews mean omit `trickplay`.

Both formats use the account's `resolve().headers`; keep tokens out of URLs.
The native loader prefetches the resume preview, caches bounded preview data
separately from posters, and uploads only the requested frame. While loading
or on failure, the player shows no preview frame or black placeholder.

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

## Pagination and collection occurrences

Pages carry opaque continuations: omit `cursor` for the first request and pass
the returned token unchanged thereafter. Only `exhausted: true` ends a listing;
short or empty nonterminal pages are valid when their cursor advances. Missing,
empty or repeated nonterminal cursors fail with `invalid_pagination`.
Collectors stop after 256 pages; collect-all also has a 10,000-row bound.
Exceeding either bound without completion reports `response_limit`.

`Item.entryId` identifies an occurrence within its container. It is not a media
ID and is never account-prefixed. Duplicate media IDs retain separate entry IDs
and queue occurrences. ID lookups fetch at most 50 unique IDs per request and
reconstruct the original requested order, including duplicates and omitting
missing rows.

## Optional extensions

Keep manifest `format: 2`, `api: "0.2"` and the existing baseline capability
names. Declare optional features in a top-level `extensions` object, for example
`{"spool.speed-test": 1}`. Values are exact positive wire-major integers
(1 through 2147483647), not minimum versions. Declarations allow at most 32
namespaced IDs of at most 128 characters. Unknown valid IDs or majors do not
prevent baseline loading; malformed declarations are rejected.

Both source and operation hosts expose the same frozen `host.extensions` map
of supported declared versions. An older API 0.2 host has no such property:
treat that as no optional support, never infer it from the application version.
Return account/server offers in `describe().extensions`; effective support is
the exact intersection of declarations, host support and account offers.
`host.emit('extensionsChanged', {extensions})` replaces only that account's
offers; support loss cancels affected calls and updates controls.

Optional operations are checked before provider execution and fail with
`unsupported_extension` when unavailable. Providers must also check support.
New speed-test implementations declare `spool.speed-test` instead of adding
the old unversioned capability. Inherited artwork owners require
`spool.artwork-owners`; omit inherited child tags on old hosts while retaining
own artwork and ordinary series/album fallback.

Keep `extensionStatus` baseline-callable and ship provider-owned notices for
old hosts. Missing host features show “Update Spool to use all features of this
provider.” Server permissions and missing endpoints are separate conditions,
not reasons to request an application update.

### Optional network facilities

`spool.http-metadata` allows `host.http` to request up to 16 response-header
names. Only those lowercase names are returned, with a 64 KiB aggregate bound;
cookie-setting headers are forbidden. Redirect and cookie policy is unchanged.

`spool.origin-grants` lets account settings/pickers request an exact HTTP(S)
origin. A host-owned confirmation names the provider, account and origin,
including an unencrypted-HTTP warning. Approval updates the worker allowlist
and persists without restarting the source; denial or stale consent grants
nothing. Certificate trust remains separate.

`spool.lan-probe` is available only to a login draft after explicit
`provider.allowLanDiscovery()` consent. `host.probeLocalHttp` probes at most
32 targets per page with four concurrent requests, a 600 ms wall deadline and
4 KiB response bodies. It scans at most two ranked private/link-local IPv4
networks (254 targets each), never public or IPv6 subnets. Opaque single-use
cursors belong to that draft's snapshot. Requests carry no credentials or
cookies and follow no redirects. Discovery does not grant authenticated access:
normal server selection still calls `allowOrigin`. Cancel/Back uses
`cancelLanDiscovery()` without disabling password or UDP login.

### Catalogue and queue contracts

`spool.suggestions` provides a bounded recommendation set. No extension means
no suggestions section; Continue Watching is not substituted. Search remains
a bounded top-N query with progressive account delivery.

`spool.item-actions` fetches permission-aware actions when a menu opens.
`spool.collection-editing` edits container-local occurrence IDs, preserving
duplicates. The host supplies both the post-removal index and preceding entry
ID for moves; the provider chooses its native move style. Unordered/read-only
containers do not offer movement. Mutations are serialized and uncertain
results trigger a refresh, not an assumed rollback.

`spool.playback-queue-reporting` adds an immutable queue snapshot only on start,
restart or membership/order revision. Ordinary progress can carry the current
index without recopying the queue. Each account receives only its own entries.
Unknown exact occurrence indexes are omitted rather than guessed.
PMS queue preparation runs on the source host and emits sanitized
`playbackQueueStatus` events; failure is nonfatal to ordinary playback reports.
Mixed audio/video queues cannot be represented by one PMS queue. Bulk append
is verified against count/order and occurrence IDs, with read-back before
reconciliation after uncertain mutations; fixture success is not a live-server
compatibility guarantee.

Protected `spool.remote-targets` state uses the same sheet/BIF descriptor in
`preview`, with an optional `headers` map. Use the same account/device
authorization as media requests; never put tokens in preview query strings.
The host keeps headers out of QML, validates the approved HTTP(S) origin and
numeric `{index}` substitution, and decodes previews through the dedicated
native loader. Authenticated previews bypass URL-only disk caching and cookies,
reject foreign-origin redirects, and keep cached data isolated by session.

### Native preferences and application data

`spool.playback-preferences` exposes the service's own audio/subtitle defaults.
Language values use ISO-639-2 (empty means no preference); the host normalizes
two-letter codes through Qt. A writable mode must round-trip its full normalized
vocabulary. Writes merge only the four mapped fields into a freshly fetched
configuration and preserve unrelated fields and user policy.

`spool.settings-storage` stores application-owned JSON documents by canonical
UUID, not filesystem or service paths. Values include JSON null; `found:false`
alone means absence. Documents are bounded to 64 KiB of compact UTF-8 JSON and
depth 16, or the provider's smaller advertised limit. Malformed, oversized or
inaccessible documents are errors, never empty documents to overwrite.

`dataInfo.conditionalWrites` describes actual server guarantees. With CAS,
`expectedRevision:null` means create-if-absent and a stale revision is
`conflict`. Replacement-only implementations reject any expected revision with
`unsupported_condition`. Jellyfin/Emby DisplayPreferences preserve the whole
DTO and unrelated CustomPrefs while changing only `spool.data.v1` in the
signed-in user's Spool partition; they advertise no CAS. Plex advertises
neither a preference writer nor application-data storage.

### Discovery and contextual screens

`host.discover({port, message, timeout})` sends the provider's UDP discovery message
on active IPv4 broadcast interfaces and returns an ordinary JavaScript array of
`{address, text}` replies. Jellyfin/Emby discovery uses broadcast, not multicast.
The host owns sockets, reply limits and cancellation; providers parse their own
protocol. Retry broadcast discovery when the viewer chooses local search, before
the optional consented `spool.lan-probe` HTTP fallback. HTTP continuation pages can
be short or empty when their wall deadline expires: follow each new cursor until
`exhausted`, rather than assuming every page scanned the requested target limit.

Provider QML is contextual. A playback `pick` result opens `ui.picker` over the
current details page with a dimmed background and returns its completed arguments
to resolution. Closing cancels the pending choice; it does not replace the details
route. Providers can compose sections within their screens (for example, an inline
Quick Connect code underneath password sign-in).

`spool.remote-targets` supplies the shared device dropdown's target, state, command
and queue data. Set a target's `customControls` when it also needs provider-owned
QML. The host mounts `ui.picker` with `{kind: "remoteControls", targetId}` as a
section inside the dropdown; `provider.complete`/`close` returns to shared device
controls. Use layouts that adapt to the available width and height. Full-page
remote controls can also open the same component as a modal overlay. Providers
never need to navigate a shell route to add these controls.
