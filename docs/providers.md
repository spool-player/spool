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
| `SourceHub` | The single `Provider` the app sees. Scopes IDs as `<8 hex of account>:<id>`, fans list requests out to every account in parallel and interleaves them, routes everything else to the owning account |
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

A provider's login screen completes with the account; the registry keeps the origins the viewer
allowed during setup, never ones the provider claims. `http_401` from any operation marks the account
as needing sign-in again. On first launch after upgrading, sign-ins saved by the old native Jellyfin
client become Jellyfin accounts.

## Where providers come from

- **Bundled**: `providers/lock.json` pins each package by SHA-256; CMake checks and unpacks it into
  a resource at configure time. `-DSPOOL_PROVIDER_OVERRIDES=id=/path/to/checkout` bundles a working
  tree instead. Pin the published release asset, so the app ships exactly what the store serves.
- **Store**: spool-player/spool-providers lists reviewed releases; first-party ids (`spool.*`) follow
  their own releases automatically, community ones change through pull requests.
- **Link**: a GitHub or GitLab project, a release's `.tar.zst`, or any site serving
  `spool-provider.json`. Updated from the same feed.

Every download is installed only when its SHA-256 matches the entry. Installing a newer version
restarts that module's accounts in place.

Downloaded code is interpreted JS and QML in the app's process, not a sandbox. That is fine for
direct downloads, Linux and webOS developer builds; a Play Store or App Store build must keep
downloading off (bundled providers only) unless the store's review has accepted the interpreter
surface. There is no build switch for that yet.

## Testing

- `providers-tests` (ctest `provider-*`, `source-hub`, `script-runtime`, `local-provider`): package
  format, registry, hub, store (against a local HTTP server) and screens, with the fixture provider in
  `tests/providers/fixtures/`.
- `bundled-jellyfin` runs a small contract against the pinned package in this Qt; each provider
  repository carries its full `tests/contract.mjs`.
- `live-jellyfin` runs sign-in to playback against a real server when `SPOOL_LIVE_JELLYFIN`,
  `SPOOL_LIVE_USER` and `SPOOL_LIVE_PASSWORD` are set; otherwise it skips.
