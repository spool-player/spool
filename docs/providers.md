# Providers

Everything Spool plays comes from a provider: a package of JavaScript and QML that knows one kind of
source (a Jellyfin server, a service, a folder). The app itself knows none of them. This page is how
the pieces fit; `sdk/README.md` and `sdk/provider.d.ts` are the provider author's side.


## Release installation flow

Spool 0.8.2 bundles only Jellyfin 0.2.3. Keeping that older bundle is intentional:
it exercises updating a preinstalled provider from the official catalogue.
Emby and Plex appear alongside Jellyfin in the startup provider chooser. Selecting
an uninstalled provider downloads, verifies and installs it, then opens sign-in.
Selecting an available update from that chooser likewise continues into sign-in.
A modal reports download progress and the verification/installation stage, then
closes automatically on success or failure. Settings retain the existing provider
update policy; background updates use the same progress display.

## Moving parts (`src/provider/`)

| | |
| --- | --- |
| `ProviderPackage` | Reads a `.tar.zst` (ustar + zstd, vendored decoder in `third_party/zstd`), validates manifest format 2 and every path, installs versions through a staging directory |
| `ProviderRegistry` | Every module (bundled at `qrc:/providers/<id>/`, installed under the data directory; newest wins) and every account. Starts enabled accounts, owns setup drafts, screens (`ProviderUiContext`) and `pick()` |
| `ScriptRuntime` / `ScriptBridge` | One worker thread and QJSEngine per module; `createSource(configuration, host)` per account; host HTTP, sockets, timers, discovery, events. Only snake_case error codes cross back |
| `PortableProvider` | One running account as a `Provider`: catalogue, search, item state, playback and artwork from its operations and URL templates |
| `SourceHub` | The single `Provider` the app sees. Scopes IDs as `<8 hex of account>:<id>`, fans list requests out to every account in parallel and interleaves them, routes everything else to the owning account. Search is planned per server (below) and shown as each answer lands |
| `ProviderStore` | `official.json` and `index.json` from spool-player/spool-providers (Pages), install by link, update checks and the `providers/updates` policy |
| `app/GroupPlaybackController` | Watching together over whichever account the group is on: clock, drift, buffering, queue handoff. Providers translate their protocol into `group` events |

`src/providers/local/LocalProvider` is the one native provider (desktop only): explicitly
selected folders combined into a library, with no implicit Movies-folder account.
Native modules may register a compiled QML root for setup/settings; their drafts do
not create a JavaScript runtime. Canonical-path item IDs distinguish same-named files
and deduplicate overlapping roots; the local cache scope is versioned for this cutover.
Local video artwork is extracted on demand through `ArtworkService`'s serial thumbnail
worker using libmpv software rendering and Qt image encoding.
Core never includes `src/providers/`; `tools/check-module-seam.sh` (ctest `module-seam`) enforces it.

## Accounts

An account is one sign-in on one provider: its module, a key the provider chose (`user@server`),
a group, a label, the origins it may reach and its configuration. Metadata lives in the durable
database (`providers/accounts/2`); configuration, which holds tokens, lives in the platform credential
store and is read off the GUI thread. Accounts in the same group (users of one server) are
alternatives: using one sets the others aside. Accounts in different groups are shown together.

Search reaches past that. Once the search page opens, set-aside accounts on a server that is in use
start too, without joining browsing, home rows or the library cache key. Each search then goes, per
server, through the fewest accounts whose libraries cover everything any of its users can see: one who
sees more stands in for one who sees less, and of two who see the same the one in use searches. Results
dedupe per server by item ID (the account in use wins), rank by how closely the title matches and then
by each server's own order, and a new query cancels the last one's operations.

A provider's login screen completes with the account; the registry keeps the origins the viewer
allowed during setup, never ones the provider claims. `http_401` from any operation marks the account
as needing sign-in again. On first launch after upgrading, sign-ins saved by the old native Jellyfin
client become Jellyfin accounts.

## Where providers come from

- **Bundled**: `providers/lock.json` pins each package by SHA-256 (Jellyfin, Emby and Plex, from
  spool-player/spool-jellyfin, spool-emby and spool-plex); CMake checks and unpacks it into
  a resource at configure time. `-DSPOOL_PROVIDER_OVERRIDES=id=/path/to/checkout` replaces matching
  pins with working trees and adds supplied provider IDs absent from the lock. Unspecified pins
  remain unchanged; empty overrides preserve release behavior. Pin published release assets.
- **Store**: spool-player/spool-providers lists reviewed releases; first-party ids (`spool.*`) follow
  their own releases automatically, community ones change through pull requests.
- **Link**: a GitHub or GitLab project, a release's `.tar.zst`, or any site serving
  `spool-provider.json`. Updated from the same feed.

Every download is installed only when its SHA-256 matches the entry. Installing a newer version
restarts that module's accounts in place.

Downloaded code is interpreted JS and QML in the app's process, not a sandbox. No store forbids
the interpreter itself (Qt runs QML without a JIT on iOS), but store review decides what downloaded
code may add. `-DSPOOL_PROVIDER_SOURCES=` chooses what a build accepts:

| Value | Store catalogues | By link | Installed from disk | For |
| --- | --- | --- | --- | --- |
| `open` (default) | yes | yes | yes | Every release build: GitHub, AUR, webOS, direct APKs |
| `curated` | yes | no | yes | Reviewed catalogues where the target store accepts this model |
| `bundled` | no | no | no | A first store submission, or a store that rejects `curated` |

A bundled build never loads installed packages or their store-origin metadata, schedules no
provider update check, and makes no store/feed requests. Provider update prompts remain empty,
regardless of a saved automatic-update preference. Existing downloaded packages are left on disk
but cannot override the bundled copies; switching back to an open or curated build restores the
usual installed-package selection.

For webOS, `build-ipk.sh` accepts `SPOOL_PROVIDER_SOURCES` and `SPOOL_PROVIDER_OVERRIDES`
as environment variables and passes them to CMake on each app configure. Overrides are a
semicolon-separated list of `id=/absolute/path` pairs. With neither variable set, each configure
explicitly restores `open` and the pinned bundles, even when reusing a build directory previously
configured for local providers. Run `./build-ipk.sh` without a phase argument for the full pipeline.

A curated build also ignores `--provider-store`/`SPOOL_PROVIDER_STORE` and never follows the feed
of a provider an earlier open build added by link. Curation is not an App Store approval guarantee:
downloaded JS/QML still needs review against Apple's downloaded-software rules and the conditions
of guideline 4.7, including native-API exposure. Initial Apple submissions should use bundled
providers, with provider code changes delivered through application updates.

## Catalogue continuation and queues

Browse pages retain provider cursors verbatim through the catalogue, hub, browse session
and warm cache. The first request omits `cursor`; subsequent requests return the last opaque
token, independently of the UI row offset. Only `exhausted` ends a listing: short or empty
pages may continue, but each nonterminal page must supply a nonempty, previously unseen
cursor or fail with `invalid_pagination`. Changing account, descriptor, filters or sort resets
the continuation.

Requested-count lists collect enough pages to fill their limit. Episodes and seasons collect
all pages, with a 10,000-row ceiling. Every collector has a 256-page ceiling and reports
`response_limit` if it cannot finish within its bounds; ordinary browse appends stay incremental.
Metadata lookup requests contain at most 50 unique IDs, with at most two requests in flight
per account. Results restore the caller's order and duplicate occurrences, omitting missing IDs.
Server-bound group and remote-play queues reject foreign account IDs with `mixed_source_queue`
before invoking the provider. Media/container IDs are account-scoped; playlist entry IDs remain
opaque within their container.

### Item menus and collection editing

Menus request `spool.item-actions` policy only when opened, using the item's owning
account. A negotiated list replaces manifest actions; older providers without that
declaration retain their type-filtered manifest list. Closing a menu cancels its
request, and execution checks the current policy again. Disabled actions retain their
reason rather than claiming that server permissions require an app update.

`spool.collection-editing` exposes **Manage entries** on playlist/collection menus.
The shared editor reads native-order pages of at most 50 entries, retaining duplicate
media as distinct opaque entry IDs. Remove and move controls follow `collectionInfo`;
smart/read-only lists cannot gain controls from another account's capabilities.
Moving down across the loaded boundary first reads the adjacent page, not the whole
container. Both `index` and `afterEntryId` describe the destination after removing the
moving occurrence.

One mutation runs at a time. Success and uncertain errors both refresh the loaded
prefix and permissions; the host never retries an uncertain mutation. Selection stays
on its occurrence, or the nearest surviving row when removed. Refreshes obey the
256-page/10,000-entry bounds and cursor checks, and support loss or account removal
cancels the editor and clears stale entries.

## Artwork ownership

Inherited thumbnails and backdrops retain their opaque `thumbItemId` and
`backdropItemId` beside the image tag. `SourceHub` scopes these IDs to the owning
account, and `ArtworkService` selects that owner rather than the child row.
Jellyfin and Emby populate them from parent-image metadata. Home payload schema
13 discards older ownerless cached rows; no account migration or data wipe is needed.

## Screens

Generic provider forms live in the app's precompiled `Spool` module:
`ServerLogin`, `ServerIdentityRow`, `ProviderLinkScreen`, `ProviderCodePanel`,
`ProviderActionPicker`, `ProviderRemoteControls`, and `ProviderCompatibilityNotice`.
Provider QML supplies protocol operation names, labels, capabilities and genuinely
service-specific flows (such as Connect membership selection and Home activation),
not duplicate form, list, navigation or PIN layouts. Providers using these forms
require the matching Spool build; there is no duplicate runtime-QML fallback.
`ServerLogin` keeps discovery, origin approval and asynchronous sign-in generations
separate; cancellation invalidates pending password/code results. Its selected
server name and address stay together above the account fields.


Every packaged `*.qml` file from the registry's selected packages (including installed overrides)
is queued for compilation on the window's own `QQmlEngine`. Warmup starts 2.5 seconds after the
first rendered frame, then schedules package enumeration and one asynchronous `QQmlComponent`
load at a time through low-priority events, with 100 ms gaps. Normal-priority foreground events
run before each queued job; Qt owns the asynchronous type-loader work. No engine or component
is moved across threads.

The cache retains compiled components, never instances: warming does not evaluate screen
bindings, run `Component.onCompleted`, start timers or authenticate. This includes every declared
UI role and nested helpers loaded dynamically, not just `manifest.ui` entries. Enumeration stays
inside selected local/resource package roots without following symlinks; validated packages
contain at most 512 files. Registry changes enqueue new packages and evict obsolete versions.
Remote URLs are not warmed, and a malformed component does not stop the remaining queue.

This is best-effort latency reduction, not a guarantee that opening a screen never compiles:
viewers may open it before warmup, and URLs outside the selected packages may still be cold.
Compilation completion still requires engine-thread work; low-priority scheduling
does not impose a real-time frame budget on Qt's compiler. Aggressive memory pressure and
shutdown cancel pending warmup and release retained components; pressure does not restart it.

Provider update prompts and toast feedback use the shared body typography and
viewport/user scale. Long feedback wraps inside content-sized panels instead
of inheriting Qt's unscaled default text size or clipping inside a fixed-height toast.

## Connection speed

New providers with a download-test endpoint declare `spool.speed-test: 1` in
`extensions` and implement `speedTest(args, host)` with `host.speedTest({url, headers})`.
The URL contains `{bytes}` and `{nonce}` placeholders; endpoint paths and authentication
stay inside the provider package. The native worker applies the account's
origin allowlist and TLS trust policy, never follows redirects, and drains
bounded buffers instead of decoding test data into JavaScript strings.

Jellyfin and Emby supply their authenticated `/Playback/BitrateTest` endpoints.
Plex instead selects an accessible media part of at least 4 MiB and requests
`host.speedTest({url, headers, range: true})`; the native worker sends bounded
byte ranges and requires HTTP 206 with an exact `Content-Range`. No stream is
played or transcoded, and a missing eligible file leaves speed unavailable.
Range mode also supports future fixed-origin file services without requiring
an artificial download endpoint. It never falls back to downloading a whole
file when a server ignores Range.

`SourceHub` probes enabled accounts one at a time after five idle seconds.
Starting playback or other foreground loading cancels an in-flight probe;
pending probes resume when idle. Account removal/restart discards its result.
Measurements are session-local. Failure leaves playback usable and keeps any
earlier successful measurement for that running account.

The benchmark warms 512 KiB and compares 4 MiB transfers over one and two
connections, adding four when latency or dual-connection improvement warrants
it. It chooses the fewest connections within 85% of the best rate, then reserves
25% headroom on that connection count. The conservative ceiling and lane count
reach `resolve` as `measuredBitrate` and `parallelRequests`; the native player
uses the same lane count. Providers must let explicit quality choices override
the measurement.

The player's Quality → Auto detail shows the active account's measured limit.
Settings → Streaming → Connection speed shows each enabled account and offers
“Measure again”; this remains deferred during playback. Providers without the
capability do not acquire a speed-test control or a measured ceiling.

Quality ceilings remain provider-neutral: explicit player bitrate, then
server-proven unlimited LAN, then the settings preference, then measured
throughput and a documented fallback. Height limits are independent. Providers
translate those ceilings into server negotiation or variant selection; remux
preferences never authorize serving an original above the ceiling. See
[`sdk/README.md`](../sdk/README.md#quality-policy) for Plex's unit conversion and
the source-selection policy for future Stremio-style providers. A catalogue
host speed test cannot stand in for an unrelated CDN or torrent route.

## Settings synchronization

Sync defaults on and selects the first eligible active account in persistent
connection order, waiting for earlier starting accounts. The chosen account is
independent of playback and is never replaced automatically on failure,
disablement, locking or removal. A source change requires confirmation because
it discards unsent intent and starts a fresh remote-first bootstrap.

Writable native audio/subtitle preferences take precedence. Other eligible
settings use the provider's optional Spool-specific document storage, never both
channels for one key. Plex has no such writer/store. Interface scale, credentials,
trust, device identity, paths, playback sessions, caches and sync controls never
leave the device. Portable settings default on; device-specific scalar settings
require per-account opt-in. System subtitle fonts require opt-in in both directions.

The Spool document uses decimal logical counters and random per-edit nonces,
not clocks or device identifiers. Retained per-key maxima repair replacement-store
races when clients reconnect, including edits overwritten after acknowledgment.
DisplayPreferences provides eventual convergence, not atomic distributed writes
or a durability guarantee for a permanently disconnected client. Real CAS stores
use their revisions; unsupported conditions are never simulated.

Local values and intent commit transactionally. Edits during bootstrap remain
provisional until a remote read establishes the counter baseline. Opt-outs keep
local values and discard unsent intent without deleting remote data. Offline
intent remains durable; malformed/future documents stop writes rather than reset
cloud data. Locale/latency retain their canonical stores and recover only unfinished
application journals, never old replicas merely because local values differ.

Changes debounce for 500 ms; one cycle runs at a time. Foreground refresh is
bounded to once per minute, failures have a 30-second automatic retry floor, and
suspended/off/locked sources do not poll. Editing rows defer remote applications;
active playback defers the four track defaults until idle or the next explicit
new-item handoff. Existing session and remembered-series selections remain prior.

Settings starts with Interface scale, Language, Sync settings and Sync account.
Native preferences use `sync`; application storage uses Material `cloud_sync`
and explicitly says “Spool-specific sync.” Green filled dots mean confirmed
sync, not an attempted write. Local-only, pending/saving, offline and error states
have distinct labels. Focus and hover show body-sized channel/account help.

Each setting remains one vertical navigation stop. Right enters its sync action;
OK toggles sync; Left/Back returns. Slider/text rows use OK to enter value editing,
where horizontal keys adjust the value/caret rather than move to sync. More sync
controls exposes opt-outs for eligible dependency/HDR-hidden and player-only
settings without duplicating reachable value editors.


## Outbound playback devices

`spool.remote-targets` is independent of inbound `remoteControl` and the local
“Allow remote control” preference. The chooser lists This device first and
loads enabled accounts progressively. Selecting a peer never starts or transfers
media. Transfer is explicit; failed remote starts leave the local queue intact.
Incoming commands, SyncPlay, automatic local advancement and CLI `--play` stay local.

Only the selected peer is polled: one second while controls are visible, three
seconds while attached but hidden, never while suspended. Queue pages are read
on demand/revision changes, or at most every five seconds for an open unversioned
queue. Optimistic commands reject older snapshots. Target IDs are account-scoped;
entry IDs remain opaque, and mixed-account remote play is rejected before dispatch.

Jellyfin/Emby queue editing replaces the target queue and therefore restarts
playback while preserving the current occurrence/position and paused state where
possible. Plex uses verified Companion identity, exact-origin consent and PMS
entry IDs. Peer playback receives only a transient delegation token, never
account/Home/PMS credentials. Missing play support leaves available transport
controls intact. Ambiguous duplicate selection is not advertised on key-only peers.
Advanced navigation/text commands remain provider-owned operations; their thin
picker adapters configure the shared, precompiled `ProviderRemoteControls` surface.

These adapters do not implement Plex watch-together or native SpoolLink peer
enhancements. Protocol/loopback verification does not imply live-device support
for an unadvertised backend command.

## Testing

- `providers-tests` (ctest `provider-*`, `source-hub`, `script-runtime`, `local-provider`): package
  format, registry, hub, store (against a local HTTP server) and screens, with the fixture provider in
  `tests/providers/fixtures/`.
- `bundled-jellyfin` runs a small contract against the pinned package in this Qt; each provider
  repository carries its full `tests/contract.mjs`.
- `live-jellyfin` runs sign-in to playback against a real server when `SPOOL_LIVE_JELLYFIN`,
  `SPOOL_LIVE_USER` and `SPOOL_LIVE_PASSWORD` are set; otherwise it skips.

The top-right playback-device menu consumes `spool.remote-targets` data and can
mount provider QML for advanced controls in place. `ProviderSurface.embedded`
omits shell page chrome for those sections; `overlay` retains the underlying page
and adds a modal scrim for playback choices and item actions. Successful login
waits for account activation before revealing the library, avoiding a return to
the provider chooser between authentication and activation.
