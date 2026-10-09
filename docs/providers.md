# Providers

Everything Spool plays comes from a provider: a package of JavaScript and QML that knows one kind of
source (a Jellyfin server, a service, a folder). The app itself knows none of them. This page is how
the pieces fit; `sdk/README.md` and `sdk/provider.d.ts` are the provider author's side.


## Release installation flow

Spool bundles the five provider versions pinned in `providers/lock.json`.
Saved accounts normally open Home. The provider
chooser is used to add a server or sign in as another viewer. Selecting an
uninstalled provider downloads, verifies and installs it, then opens sign-in;
selecting an available update likewise continues into sign-in.
A modal reports download progress and the verification/installation stage, then
closes automatically on success or failure. Settings retain the existing provider
update policy; background updates use the same progress display.
For a package-schema cutover, verification may bundle archives built from the
canonical provider checkouts. Release promotion must publish those exact bytes
and replace the curated catalogue entries with the real release URLs and digests;
working packages must not be described as already published releases.

## Moving parts (`src/provider/`)

| | |
| --- | --- |
| `ProviderPackage` | Reads `.szo` (ustar compressed with official libzstd), validates manifest format 3 and every path, stages inert files before registry admission and atomic activation |
| `ProviderRegistry` | Every module (bundled at `qrc:/providers/<id>/`, installed under the data directory; newest wins) and every account. Starts enabled accounts, owns setup drafts, screens (`ProviderUiContext`) and `pick()` |
| `ScriptRuntime` / `ScriptBridge` | One worker thread and QJSEngine per module; `createSource(configuration, host)` per account; host HTTP, sockets, timers, discovery, events. Only snake_case error codes cross back |
| `PortableProvider` | One running account as a `Provider`: catalogue, search, item state, playback and artwork from its operations and URL templates |
| `SourceHub` | The single `Provider` the app sees. Scopes IDs as `<8 hex of account>:<id>`, fans list requests out to every account in parallel and interleaves them, routes everything else to the owning account. Search is planned per server (below) and shown as each answer lands |
| `ProviderStore` | `official.json` and `index.json` from spool-player/spool-providers (Pages), install by link, update checks and the `providers/updates` policy |
| `app/GroupPlaybackController` | Watching together over whichever account the group is on: clock, drift, buffering, queue handoff. Providers translate their protocol into `group` events |

### Current capability contract

Format 3 is the single package schema boundary; manifests and feed entries do
not carry a separate API version or extension wire-major map. A manifest's
`capabilities` is a bounded list of known feature names. The worker exposes
those declarations as the frozen boolean map `host.capabilities`.

Each account's required `describe()` operation returns its current boolean
`capabilities` offers. Only a declared capability explicitly offered as `true`
is enabled; absent offers are disabled. Providers replace their account offers
with `host.emit('capabilitiesChanged', {capabilities})`. The registry updates
native feature flags and provider screens together, cancels affected operation
scopes, and rejects results from a withdrawn capability even if it is offered
again before completion. `callSource`, `callSourceMediaPage` and `callSourceItem`
are the single operation boundary: optional operations are guarded before
provider execution, including the validated native preferences and settings
storage paths. Unsupported operations fail with `unsupported_capability`.
Joined group playback also follows its owning account, not aggregate availability:
withdrawing that account's `groupPlayback` clears the active group, synchronization
timers and pending group handoff so playback controls return to local ownership.
Another account offering group playback cannot retain the withdrawn group's state.

Capabilities advertise behavior, not permission: per-account server policy,
private activation approval, explicit origin/LAN consent, cancellation and
conditional storage writes remain independent guards. Provider screens read
`provider.capabilities`; login drafts see declarations and live account screens
see effective offers. Closed screens see no capabilities. Providers and host
cut over together; old package schemas are not adapted or negotiated.


`src/providers/local/LocalProvider` is the one native provider: explicitly
selected desktop folders form a library, with no implicit Movies-folder account.
The download service also supplies a dedicated **Downloads** instance on every
platform, using its durable inventory (including Android content documents)
and credential-free media metadata. That inventory remains usable offline and
after removing the originating account; local playback progress persists in
the application data directory.
Native modules may register a compiled QML root for setup/settings; their drafts do
not create a JavaScript runtime. Canonical-path item IDs distinguish same-named files
and deduplicate overlapping roots; the local cache scope is versioned for this cutover.
Local video artwork is extracted on demand through `ArtworkService`'s serial thumbnail
worker using libmpv software rendering and Qt image encoding.
Core never includes `src/providers/`; `tools/check-module-seam.sh` (ctest `module-seam`) enforces it.

`DownloadSource` negotiates original or finite server-transcoded media independently
of playback `resolve` and reporting. Manifest `downloads` and `downloadTranscode`
capabilities gate choices per owning account; the latter is not inferred from
ordinary streaming quality. `DownloadManager` streams bounded native chunks into
incomplete files/documents, cancels requests, applies account origin/TLS policy,
rejects redirects/playlists, and commits a complete offline inventory only at EOF.
Server session cleanup is memory-only and calls `downloadRelease` after terminal
transfers. See `sdk/README.md` for the exact operation and picker contract.
Download negotiation scopes also own pending provider pickers. Cancelling a
download closes only its picker and rejects stale answers before any second
server operation; ordinary playback and concurrent download pickers are unchanged.
Closing the provider's own choice screen also produces a terminal Cancelled job,
not a server failure. Preparing status covers both viewer choice and negotiation.

Library grids request rendered artwork only for tiles intersecting the viewport.
Buffered and pooled delegates do not occupy the render queue; scrolling cancels
out-of-view loads so newly visible tiles can load without waiting for earlier rows.
Speculative artwork prefetch remains separate from visible render requests.

## Accounts

An account is one sign-in on one provider: its module, a key the provider chose (`user@server`),
a group, a label, the origins it may reach and its configuration. Metadata lives in the durable
database (`providers/accounts/2`); configuration, which holds tokens, lives in the platform credential
store and is read off the GUI thread. Accounts in the same group (users of one server) are
alternatives: using one sets the others aside. Accounts in different groups are shown together.

Viewers that are alternatives to one another form a **profile set**: one
provider activation family (such as a Plex Home, which spans its servers) or,
without one, one provider group (the users of one server). One person per set
watches at a time; independent sets appear on Home together. Choosing a person
in a family brings their other saved servers along, authorized by the in-memory
family proof rather than another PIN. Providers own sign-in and any PIN; the host
only groups what they describe and never assumes a Home API.

Each set has a device-local startup choice, persisted with the account metadata.
After adding any provider, select the watching profile, then choose **Always use
this profile** or **Choose a profile at startup**. This includes single-user and
account-free providers. Without a choice the last-used viewer opens. A pinned profile
opens even when another was used later, but the provider still sees the honest
last-used flag, so a protected profile stays locked rather than skipping its PIN.
Only the active, authorized viewer can be pinned. When a set asks, its viewers stay
stopped and **Who's watching?** opens at launch for those sets only; other sets
start normally. The chooser precedes recovered routes after shell close, memory
reclaim or a crash; Home cannot bypass an unanswered set. Startup activation never
opens a PIN chooser.
The preference applies to the viewer actually selected, including a previously
saved viewer chosen instead of the newly added account; finishing clears the
new account's onboarding marker.

Installed providers are reachable from **Profiles & servers** and each profile set's
**Provider settings** button. The provider detail page shows the installed package
version (not a server version), recorded installation provenance and update status,
then saved accounts identified by account ID. Account settings mount the provider's
existing settings surface; opening this page never activates a locked viewer.
Reconnect, profile activation and PIN approval remain explicit provider-owned flows.
Watching-profile tiles and linked login choices carry the installed provider logo
at bottom-left and version at bottom-right, with complete provider/version text for
assistive technology. Compact login tiles retain their lock badge above the stamps.

**Profiles & servers** shows each set with its server(s), its startup choice and
Add profile, then one tile per person. Tiles carry one short state (Watching,
Opening…, PIN required, Couldn't open, Sign in again, Removing…). An actionable
failure appears once beside its profile as an accessible alert, never as a raw
provider code or duplicated toast. A pending switch can be cancelled with Back or
the remotely focusable Cancel button, leaving the
current viewer unchanged. Every tile has a menu (Menu key, hold, right-click) with
Remove, even while its activation is pending or has failed; removal cancels the
pending activation and never requires a successful one. Selection returns Home only
after successful activation. Search uses these same selected identities, never
saved alternative profiles or a union of their permissions.
Server controls wrap below their label at narrow widths. Startup preference
buttons stay entirely visible, with Up/Down as well as Left/Right remote navigation.
Login admission opens that profile immediately, so unfinished or failed activation
can be cancelled, retried or removed without waiting behind a login spinner.
An incompatible catalogue update does not block signing in to an already installed,
validated provider; the incompatible package itself still cannot be installed.

Add profile and Sign in again carry the selected server into setup; the viewer
does not re-enter its address. `beginSetup(moduleId, accountId, purpose)` validates
that the selected account belongs to the provider. Login QML receives only
`arguments.setupContext` (account/server identity, public origin and purpose).
The provider factory privately receives `setupAccount`, its own retained
configuration, and decides whether household credentials can be reused. Reconnect
must return the saved account/server identity. Draft `configuration` events store
credentials privately until setup commits; they need not travel through QML.
Removal shows its state immediately and settles locally within three seconds even
if best-effort server sign-out never answers.
Approving another server extends the existing login draft in place: private
link/member selection and retained provider state survive origin approval. This
login authority does not require the optional saved-account `originGrants` offer.

Changing the browsed account set immediately removes unavailable accounts from the
library list, Continue Watching, Next Up and Recently Added while retaining the
remaining accounts' rows. Home content then refreshes with the current libraries.
Pending home/cache and library-list responses from an older account set cannot
restore removed content.

Library visibility and ordering are local presentation preferences keyed by
opaque scoped library IDs. Hiding a library does not change the retained catalog,
Continue Watching, Next Up, or all-media query scope. Ordinary mouse dragging
scrolls the library row. Right-click or hold opens Move, Hide library, and Show
hidden libraries; only explicit Move enables reordering. Hidden libraries can
be restored individually from this menu or the library heading, even when all
libraries are hidden. Direct navigation resolves IDs against the full catalog.

Episode completion immediately removes the played episode from Continue Watching
and Next Up, replacing it with a known chronological successor from that same
account-scoped series when available. After the played mutation is acknowledged,
the two rows refresh from the server. Older home/cache requests and eventually
consistent responses cannot restore the completed episode or erase a newer local
resume update.

Search dedupes same-server item IDs, then dedupes across independent sources using
database identifiers scoped to the media type. Identifier namespaces are case-insensitive;
IMDb, TMDb and TVDb values normalize whitespace, supported URL/protocol forms and numeric
padding. Matching identifiers merge translated titles, but conflicting identifiers in any
shared database keep items separate. Movies and series without a conflicting identifier
can also match an identical case-insensitive title and known year; punctuation and accents
are not removed. Episodes and seasons never merge by title alone, and unknown years do not
trigger title fallback. Results rank by how closely the title matches and then by each
server's own order. The best-ranked original item retains its source-scoped activation and
artwork; its provider icon and server label appear on every search card. A new query cancels
the last one's operations.

A provider's login screen completes with the account; the registry keeps the origins the viewer
allowed during setup, never ones the provider claims. Authentication and incomplete-configuration
failures (`http_401`, `auth_required`, `invalid_token`, `invalid_config`) expose an actionable
sign-in state without saving failed candidate credentials. Incorrect PIN answers stay retryable.
On first launch after upgrading, sign-ins saved by the old native Jellyfin client become Jellyfin accounts.

## Where providers come from

- **Bundled**: `providers/lock.json` pins each package by SHA-256 (Jellyfin, Emby, Plex,
  Stremio and Open Movies, from their `spool-player` repositories); CMake checks and unpacks it into
  a resource at configure time. `-DSPOOL_PROVIDER_OVERRIDES=id=/path/to/checkout` replaces matching
  pins with working trees and adds supplied provider IDs absent from the lock. Unspecified pins
  remain unchanged; empty overrides preserve release behavior. Pin published release assets.
- **Store**: spool-player/spool-providers lists reviewed releases; first-party ids (`spool.*`) follow
  their own releases automatically, community ones change through pull requests.
- **Link**: a GitHub or GitLab project, a release package link, or any site serving
  `spool-provider.json`. Updated from the same feed. Future packages use `.szo`
  (Spool Zstandard Object); existing published `.tar.zst` URLs remain valid transport names.
- **Local file** (open builds only): inspect a bounded package without executing JS/QML,
  loading candidate icons, starting accounts or changing the installation. Host confirmation
  shows supplied, unverified publisher text, installed → incoming version, provenance,
  declared capabilities/network scope and their differences, and code/credential trust.
  Cancel invalidates consent without changing installed code or saved accounts.

Every download is installed only when its SHA-256 matches the entry. Installing a newer version
restarts that module's accounts in place.

`Store.inspectFile(fileUrl)` returns a request ID and asynchronously reports
`fileInspectionFinished(requestId, preview, error)`, also publishing `inspectedPackage`.
The preview's opaque token binds exact validated contents and the installed-provider revision.
`installInspected(token)` consumes that approval without rereading the pathname, and reports
`fileInstallationFinished(token, moduleId, error)`. A changed pathname cannot swap approved bytes.
`cancelInspection(operationId)` cancels only its matching inspection/consent/install; omitting
the argument cancels the current operation. Still-staging cancellation prevents activation.
Staging never mounts candidate code; the registry rechecks lifetime, per-module revision and
consent immediately before activation. Local transfer rows carry `operationToken` and actual
committed staging-file byte counts. Consumers retain modal input ownership until their matching
completion, never advancing from unrelated global `problem`/`installed` signals.

A matching module ID, claimed publisher or supplied digest is not distribution authentication.
Unverified local files cannot replace native, bundled, official, community, URL or unknown
installed identities. Only an existing file-installed provider may receive a newer local-file
version, with renewed explicit consent every time. Saved accounts, their credentials,
session identity and approved origins remain registry-owned; install approval is not login,
PIN, LAN or network-origin approval. File providers are never automatically updated from
a catalogue/feed, and installation does not change the device's update policy.

`Store.classifyFiles(urls)` returns a request ID and reports
`filesClassified(requestId, packages, media, error)` after bounded worker-thread content sniffing.
Special files are rejected before opening; Unix nonblocking open and handle validation also
reject a regular-file-to-FIFO substitution. Neither package decoding nor file reads run on
the GUI thread. `isPackageCandidate(fileUrl)` is a suffix-only `.szo` naming hint, not content
validation or trust. Malformed candidates remain inspection errors, not fallback media.
`Store.installedProviders` exposes only module IDs and safe provenance channel names.

Stremio owns its add-on setup/settings and stream/torrent-file picker. It reads
trusted Stremio add-on catalogues and metadata; HTTP streams play directly,
while torrent choices require a configured external Stremio-compatible streaming
server. Spool does not embed a torrent engine. Connection origins are approved
explicitly, including discovered add-on catalogue redirects, without wildcard
grants. Stremio offers Original downloads only for finite files, not HLS/DASH
playlists or server-converted quality options.

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

Apple App Store builds additionally set `-DSPOOL_APPLE_APP_STORE=ON`. This forces
`bundled` provider loading and includes only pins whose `appleAppStore` field is
explicitly `true`; missing or false approval excludes a provider. Checkout overrides
are rejected, so a local checkout cannot bypass the reviewed release pin.
The curated catalogue maintains the same required boolean independently of provider
release feeds; pin updates must copy that reviewed value into `providers/lock.json`.
Jellyfin, Emby, Plex and Open Movies are included. Stremio is excluded by default.

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

Normal episode EOF advances the local queue unpaused. If no queued successor is
available, Spool expands that episode's account-scoped series context and selects
the next playable episode. Explicit Stop or replacement playback cancels pending
lookup and negotiation. Queue edits retain a pending lookup or immediately advance
an explicitly queued successor; a final episode ends the transition without replay.
Group playback remains server-authoritative and can deliberately start paused.

Stream metadata uses a nonnegative file index or `-1` for unindexed analysis and
sidecars; no other negative metadata integer is accepted. Providers normalize
decimal-string frame rates and ratings to finite numbers, and preserve exact
signed-64-bit ticks and byte sizes as decimal strings.

### Item menus and collection editing

Menus request `itemActions` policy only when opened, using the item's owning
account. Its current list replaces manifest actions; packages that intentionally
use only static manifest actions retain their type-filtered list. Closing a menu cancels its
request, and execution checks the current policy again. Disabled actions retain their
reason rather than claiming that server permissions require an app update.

`collectionEditing` exposes **Manage entries** on playlist/collection menus.
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

The QML artwork boundary accepts native items, JavaScript values and materialized
item maps without dropping series/album artwork, thumbnails, backdrops, logos,
banners or their owner IDs. `artwork-integration` renders loopback images through
the real image provider before and after first population, row recreation and
card resizing, and checks the rendered pixels as well as inherited ownership.

## Screens

Generic provider forms live in the app's precompiled `Spool` module:
`ServerLogin`, `ServerIdentityRow`, `ProviderLinkScreen`, `ProviderCodePanel`,
`ProviderActionPicker` and `ProviderRemoteControls`.
Provider QML supplies protocol operation names, labels, capabilities and genuinely
service-specific flows (such as Connect membership selection and Home activation),
not duplicate form, list, navigation or PIN layouts. Providers using these forms
require the matching Spool build; there is no duplicate runtime-QML fallback.
`ServerLogin` keeps discovery, origin approval and asynchronous sign-in generations
separate; cancellation invalidates pending password/code results. Its selected
server name and address stay together above the account fields.
Login action buttons use a blue fill, including a darker blue disabled state,
so actions remain visually distinct from charcoal username/password fields.


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

Providers with a download-test endpoint declare and offer `speedTest` in
`capabilities` and implement `speedTest(args, host)` with `host.speedTest({url, headers})`.
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

Settings presents one **Settings sync** entry in Accounts rather than repeating
connection badges beside each preference. Its dedicated page shows the selected
account, one overall status, and an appropriate sign-in, profile switch, account
selection or retry action. Switching profiles explicitly warns that it changes
who is watching on that server; account changes retain the existing confirmation.
The account picker includes inactive capable accounts using their manifest
declarations, with a state label, without pretending retry can reconnect them.

The **Recommended** preset includes portable preferences. Streaming limits, like
other device-specific settings, require opt-in. **What syncs** discloses category
toggles only on request; category updates invalidate and persist once, retaining
per-account consent. **Use recommended settings** clears that account's overrides
and performs a fresh remote-first bootstrap for affected keys. Hidden/playback
preferences are included by category without duplicate value editors.

Normal settings and subtitle appearance retain one vertical navigation stop per
value. Horizontal keys edit values rather than entering a sync badge. OK/Back
enter/leave slider or text editing; edit locks still defer remote applications.
The sync page's recovery buttons support D-pad traversal into and out of its
settings list. Buttons and setting/toggle rows expose accessible names, state
and activation. Host login uses one primary action with local feedback and
secondary alternatives. Account/provider removal requires a Cancel-first
confirmation; destructive provider action pickers also start on Cancel.


## Outbound playback devices

`remoteTargets` is independent of inbound `remoteControl` and the local
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

Protected playback previews use account-scoped `spool-artwork://` resources,
resolved privately by `SourceHub`. `TrickplayService` loads local and remote
sprite sheets or whole BIF sequences on a dedicated worker. Sheets fitting
the 50,000,000-byte decoded RGB32 budget expose stable sheet `image://` URLs
and per-thumbnail crop offsets; larger JPEGs and BIF use single-frame URLs.
Local descriptors use the owning account's playback headers; remote descriptors
may supply their account/device `headers`.
Remote previews do not require starting local playback. Requests stay on the
approved origin, reject foreign-origin redirects, disable cookies and bypass
Qt's URL-only disk cache. Encoded and decoded texture caches are bounded and
session-isolated; removed accounts cannot resolve old resources, even from memory.
Directional prefetch is limited to one neighbouring sheet or two BIF frames
and yields to foreground requests. Both preview surfaces consume the latest
selection once per rendered frame and clip resident sheets to the selected tile.

Raw JPEG decoding links the codec selected by the Qt toolchain. Official SDKs
using Qt's bundled libjpeg expose its C API at `QtJpeg/jpeglib.h`, through
`Qt6::BundledLibjpeg` and `Qt6::JpegPrivate`; system-codec builds use `jpeglib.h`
and `JPEG::JPEG`. Keep the headers paired with their selected library—do not
copy unrelated JPEG headers or disable planar decoding for a platform.

These adapters do not implement Plex watch-together or native SpoolLink peer
enhancements. Protocol/loopback verification does not imply live-device support
for an unadvertised backend command.

## Testing

- `spool-tests --child <selector>` drives native provider contracts for package
  format, registry, hub, store (against a local HTTP server) and the fixtures in
  `tests/providers/fixtures/`. `spool-e2e-tests` owns GUI/provider-screen contracts.
  Run the common supervisor with
  `python3 tools/run-tests.py --build-dir <dir> --workers N`; all traditional
  selectors settle before the GUI e2e phase begins.
- `bundled-jellyfin` runs a small contract against the pinned package in this Qt; each provider
  repository carries its full `tests/contract.mjs`.
- `live-jellyfin` runs sign-in to playback against a real server when `SPOOL_LIVE_JELLYFIN`,
  `SPOOL_LIVE_USER` and `SPOOL_LIVE_PASSWORD` are set; otherwise it skips.

The top-right playback-device menu consumes `remoteTargets` data and can
mount provider QML for advanced controls in place. `ProviderSurface.embedded`
omits shell page chrome for those sections; `overlay` retains the underlying page
and adds a modal scrim for playback choices and item actions. Successful login
waits for account activation before revealing the library, avoiding a return to
the provider chooser between authentication and activation.
