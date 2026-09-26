# Providers

Everything Spool plays comes from a provider: a package of JavaScript and QML that knows one kind of
source (a Jellyfin server, a service, a folder). The app itself knows none of them. This page is how
the pieces fit; `sdk/README.md` and `sdk/provider.d.ts` are the provider author's side.

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

`src/providers/local/LocalProvider` is the one native provider (desktop only): a folder of files.
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
  a resource at configure time. `-DSPOOL_PROVIDER_OVERRIDES=id=/path/to/checkout` bundles a working
  tree instead. Pin the published release asset, so the app ships exactly what the store serves.
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
| `curated` | yes | no | yes | Google Play and App Store submissions |
| `bundled` | no | no | no | A first store submission, or a store that rejects `curated` |

A curated build also ignores `--provider-store`/`SPOOL_PROVIDER_STORE` and never follows the feed
of a provider an earlier open build added by link. Google Play allows interpreted code loaded at
run time as long as it can't be used to break Play policy; Apple allows JavaScript plug-ins under
guideline 4.7 when the app answers for every one of them (an index, reporting, age limits, and
no native APIs exposed to them without Apple's permission). Curation through the store's pull
requests is what makes that answerable.

## Connection speed

Providers with a download-test endpoint declare `speedTest` and implement
`speedTest(args, host)` by calling `host.speedTest({url, headers})`. The URL
contains `{bytes}` and `{nonce}` placeholders; endpoint paths and authentication
stay inside the provider package. The native worker applies the account's
origin allowlist and TLS trust policy, never follows redirects, and drains
bounded buffers instead of decoding test data into JavaScript strings.

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

## Testing

- `providers-tests` (ctest `provider-*`, `source-hub`, `script-runtime`, `local-provider`): package
  format, registry, hub, store (against a local HTTP server) and screens, with the fixture provider in
  `tests/providers/fixtures/`.
- `bundled-jellyfin` runs a small contract against the pinned package in this Qt; each provider
  repository carries its full `tests/contract.mjs`.
- `live-jellyfin` runs sign-in to playback against a real server when `SPOOL_LIVE_JELLYFIN`,
  `SPOOL_LIVE_USER` and `SPOOL_LIVE_PASSWORD` are set; otherwise it skips.
