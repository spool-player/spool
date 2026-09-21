# Spool: portable providers without compromising the native fast path

**Implementation brief · 21 September 2026**
**Baseline:** `refactor/provider-seam`, the supplied source ZIP, and the supplied September 18–20 commit export.
**Decision:** Qt JavaScript provider logic, provider-owned QML, native Spool services/models/rendering/playback, independently published source packages.
**Status:** A plan, not a report of completed implementation, device benchmarks, repository creation, or store approval.

## 1. Start here: objectives and authority to adapt

Spool is an unusually performance-sensitive media client. The provider system must extend that product, not turn its native UI into a slow scripting framework. Optimise for the weakest supported LG webOS television, not a developer workstation. Preserve libmpv playback, platform/HDR integration, fast startup, responsive navigation and consistently smooth scrolling.

Deliver one application that can use multiple first- and third-party providers, multiple accounts, and multiple servers concurrently. It must search them together, progressively group equivalent titles, and let users select an exact available version. A selected browsing source is not a global backend switch.

Provider authors maintain one implementation: JavaScript API/client logic plus optional QML screens. Providers own backend-specific behaviour and bespoke UI, including Stremio add-on configuration, release/torrent/file selection and preparation workflows. Core provides generous reusable functionality, not provider-specific pages. Shared library providers, Wasm, a second maintained native provider implementation, and a new web renderer are **not** part of this implementation.

Ship official providers with the application, at least Jellyfin, so setup and normal operation do not depend on downloading code. Create separate `spool-jellyfin` and `spool-stremio` repositories. Support installing/updating compatible provider source packages independently where the distribution channel permits it, including a deliberate Google Play path. NixOS support can use the Linux AppImage through `appimage-run`; native Nix provider packaging is not a priority.

**Treat this document as an implementation brief, not a frozen class diagram.** Inspect the actual checkout first. Preserve the requirements and observable behaviour, but change names, internal interfaces, thread allocation, schema details, milestone ordering and repository layout when evidence supports a better solution. Record consequential deviations and their tests in a short decision log. Do not ask for approval on routine engineering choices or stop after producing another plan.

Rewrites are inexpensive at this stage. Retain useful behaviour, performance work, tests and architectural separation—not code merely because someone wrote it recently. Do not build elaborate compatibility shims around unsuitable single-source or Jellyfin-shaped abstractions. Conversely, do not rewrite the proven renderer/player simply to make the provider migration aesthetically uniform.

### Fixed performance pipeline

```text
Native controller requests a bounded page
    -> provider JavaScript constructs the backend request
    -> native asynchronous HTTP
    -> provider JavaScript parses/normalises the response off the GUI thread
    -> one conversion into owned native page data
    -> bounded incremental updates to the native model on its owning thread
    -> existing Qt Quick delegates render the native model
```

“One conversion” means no avoidable stringify/parse round trip or repeated full-object conversions. It does not mean zero allocations, zero copies, or one giant GUI-thread transaction. HTTP decompression, text decoding, parsing, normalisation, conversion and rendering must all be measurable stages.

Once data is in the model, reading rows/roles and rendering existing items must not call provider backend JavaScript. Provider-owned UI may use ordinary lightweight QML bindings and event handlers; it must not move bulk data processing into those bindings.

## 2. Performance contract and evidence

### Goals, not retrospective claims

Target **under 100 ms to the first useful page frame** on the minimum supported webOS device. Define that for each page/workload and report at least median, p95 and tail behaviour. For a cached page, measure input/navigation to the frame. For a remote page, separately measure request latency and response-ready to the frame. Also report true end-to-end latency; do not hide slow request orchestration by only measuring rendering.

Network/server delays and uncached artwork downloads are not under a 100 ms guarantee. A skeleton counts as immediate feedback, **not** a completed content page. Standard pages, provider-specific pages and cold component activation all need measurements; do not meet the target by quietly excluding every cold path.

Use the following as initial engineering budgets, adjustable with recorded measurements rather than silently relaxed:

| Area | Starting expectation |
| --- | --- |
| GUI model commits | Usually about 1–2 ms of blocking work per event-loop turn; split expensive batches without delaying the first visible rows. |
| Ordinary provider page | A provisional background processing budget around 10–30 ms for roughly 50–100 lightweight items / 100–300 KB of JSON. This is an unmeasured hypothesis, not an LG guarantee. |
| Frame pacing | At a 60 Hz presentation target the whole frame budget is about 16.7 ms; provider activity should cause no material regression in baseline frame pacing or input response. Measure the actual presentation rate. |
| Warm navigation | Reuse native normalised pages and compiled/cached UI resources when valid; avoid invoking providers merely to redisplay cached data. |
| Idle cost | No periodic provider polling or per-frame backend work without a real feature requirement. |

Qt recommends asynchronous processing, worker threads and short blocking functions; its performance guidance also identifies conversions and model design as potential costs. These support the direction, not the numerical Spool budgets above. [R2]

Identify the actual minimum hardware/firmware from the project's support policy and available devices; do not assume all LG TVs, or all webOS versions, perform like a C2. If the device is unavailable, implement repeatable instrumentation and report the device acceptance gate as pending. Do not substitute desktop timings for TV validation.

Instrument request scheduling, first/last response bytes, decompression/decoding, JSON parsing, mapping, conversion, aggregation, queued-delivery delay, model commits, delegate creation and first useful frame. Track memory, allocation/GC pressure, queue depth and concurrency. Reuse the existing `RenderBenchmark`/input-latency machinery and fixture infrastructure where useful.

Exercise small pages, broad searches, multi-megabyte responses, thousands of stream candidates, many simultaneous source completions, rapid query changes, pagination while scrolling, and browsing during high-bitrate playback. Benchmark interpreter-only execution as well as the normal shipped engine mode. Keep payload fixtures free of credentials and licensed appropriately.

Protect responsiveness through bounded responses, server-side pagination/filtering when available, low-overhead batch conversion, virtualised delegates, fair background scheduling and memory limits. Async HTTP alone is not enough. Do not fetch complete libraries or every media track just to show a page; do not truncate an unpaginated service response and falsely report completion. Use explicit continuation, local batching or a visible limit as appropriate.

## 3. What the branch contributes—and what to replace

The supplied commit export documents the work below. Its descriptions and the existing plan are evidence of intended changes, not proof of runtime correctness or performance. The supplied source snapshot was also inspected for this plan. The implementing agent must reconcile these notes with the actual branch; the old document's “77 tests pass” statement is not independent validation in this document. [S1–S3]

| Work / commit reference | Reuse decision |
| --- | --- |
| `adc13dbe`: `MediaTypes` moved out of Jellyfin types | Retain the native media-model foundation and useful metadata; audit fields and identity. The commit explicitly made no field changes, so relocation is not proof of provider neutrality. |
| `b9c5bec2`: `PlaybackSource`, token/header handling outside player | Retain dependency inversion, playback behaviour and tests. Replace source-global assumptions with resource/session-scoped requests. |
| `d021a76c`, `c8bf5f16`: catalog/search/item-state/artwork interfaces and core controllers | Retain native models, controllers, caching and test expectations where appropriate. These are internal adaptation points, not a frozen public script API. |
| `d45bcb52`: settings inversion | Keep the event/bridge separation. Backend settings operations belong in provider JS; remove direct backend dependencies from core. |
| `cc235686`, `c9afbcb7`: platform capability applier and application/benchmark hooks | Keep the platform and diagnostics separation. Generic host capability descriptions should replace backend-specific consumers without reintroducing coupling. |
| `542810b6`, `985829dc`: provider composition and `AppController` separation | Keep the ownership lesson and behavioural coverage. Replace broad concrete core-controller exposure, global singletons and captured “active provider” pointers. |
| `61cb6fb1`, `bf37ebe2`: provider QML moved/gated; focus-navigation tests | Retain reusable pages and navigation tests. Gating hardcoded Jellyfin globals is not a general source-aware UI integration. |
| `611b2e27`: registry/capability scaffolding | Rework into installed modules, configured source instances and per-operation contexts. A registry that selects one active provider is not the required multi-source registry. |
| `20bc39f4`: folder-of-files provider | Keep an offline fixture permanently. Add a portable script fixture exercising the actual runtime, and preserve local playback through host-authorised file access when useful. |
| `5abb2e7f`: seam checks and build grouping | Keep and strengthen the checks. Cover QML imports/routes, script imports and actual independently packaged providers, not just C++ includes. |
| `e0de68c1` / `4ad4b560`: split documentation | Replace stale architecture/status sections with this direction and measured implementation status. |

Concrete snapshot issues to resolve include a `Provider::CoreServices` bundle of concrete controllers; one active provider wired into `AppController` and artwork; hardcoded Jellyfin route URLs/singletons; a single-source, finite-vector search interface; and playback resolution without an explicit selected variant. The archive also still combines core and provider sources in `jellyfin-core`. Treat these as inspection leads, not immutable line-number instructions.

Remove the native plugin loader/build-matrix workstream, exact Spool-build provider indexes, mutual repository dependencies and the assumption that Catalog/Stream/Artwork must all be mandatory. Do not preserve the old C++ Jellyfin facade indefinitely behind a script-shaped forwarding layer: that would defeat independently updateable API logic. It may serve as a temporary reference and differential-test baseline during migration.

## 4. Native host and worker runtime

### Responsibilities

| Native Spool | Portable provider |
| --- | --- |
| Async transport, TLS, bounded downloads, cancellation, credentials/storage services | Endpoint paths, parameters, authentication protocol, provider-specific error interpretation and retry decisions |
| Efficient typed media/custom list models, caching and incremental commits | Response parsing/normalisation and provider-specific data |
| Federated search, identity grouping, generic version chooser | Source-local queries, external IDs, version discovery and exact-source resolution |
| libmpv, media I/O, buffering, decode, HDR, timing and rendering | Playback negotiation, reports and backend-specific session messages |
| Generic routes, focus/navigation, reusable UI kit and UI-to-worker RPC | Bespoke QML pages, dialogs, actions and workflows |
| Device capability discovery and reusable selection policies | Translation to backend-specific device profiles/requests |

A host feature should be reusable or genuinely performance/platform critical. Do not grow `host.jellyfinSearch()` or move entire backend response parsers into core to avoid rewriting them. Equally, a small generic native helper justified by measured cost is acceptable. Do not invent a universal media framework before the two real providers need it.

### Engine and thread model

Use a backend `QJSEngine` execution context separate from the frontend `QQmlEngine`. Keep each engine and its JS values confined to its owner thread; schedule calls through queued work, never concurrently into one engine. Engine count and thread count are implementation choices. A reasonable starting point is an isolated module execution context assigned to a small, bounded set of workers, with explicit source-instance objects inside it. Measure memory before creating one engine per account, request or page.

JavaScript modules are cached/singleton-like within an engine. Create explicit account/source objects rather than putting “current token/current server” in module globals. Qt documents module caching, native object bridges and an interrupt facility; none of those makes arbitrary concurrent engine access safe. [R1]

Expose small source-bound host facades, not raw `PlayerController`, `SettingsController`, models or arbitrary QObject trees. Calls to native networking/storage must execute on their correct threads; do not wrap a foreign-thread QObject and then invoke it synchronously from JS. Track request ownership by module, source, source generation and operation identity.

Retain the frontend's coroutine-shaped interface. An internal script adapter can implement useful native interfaces with `QCoro::Task<T>`-style results. It must settle each operation exactly once, preserve errors/cancellation, and resume work in the appropriate context. This is implementation work, not an automatic `QJSValue` to QCoro conversion. A QML-facing task adapter may be useful for small UI actions, but the main listings still consume native models. [R7]

Specify a small portable JavaScript language/host profile and test it using the actual bundled Qt engine. Bare QJSEngine is not Node.js or a browser. Provide timers, native async HTTP, events and storage explicitly. Verify Promise/module/async syntax support before committing to author-facing examples. Prefer one small idiomatic async SDK; optional TypeScript may compile to the supported JS baseline, but must not require a second runtime or a large compatibility framework. [R1, R6]

### Data path and lifetime

Perform JSON parsing and normalisation on a worker, then validate and convert into owned native DTOs there. Send those values to the GUI-thread model without live JS objects or cross-thread JS heap references. Update QAbstractItemModel only on its owning thread. [R3]

Default to JS parsing/mapping with a single native conversion, but profile actual representation costs. Avoid parsing in C++, materialising a full JS copy, stringifying it and parsing it in C++ again. Native buffers may be shared or moved when safe; correctness and lifetime take priority over speculative zero-copy claims.

Use strings for opaque IDs, explicit time units and well-defined optional/unknown values. Validate safe integer ranges for file sizes, timestamps and backend counters; preserve large values losslessly where needed rather than silently rounding them through JavaScript numbers. Do not carry every backend's raw payload indefinitely inside every media item.

Cancellation includes aborting native requests when possible, suppressing late results, releasing completion handlers and preventing use-after-free on source removal. Invalidate a search generation immediately when its text changes, not when the debounce timer eventually submits the next request. Disconnecting one account must not cancel or reroute other accounts.

Provide limits on response bytes after decompression, outstanding work, per-source concurrency, stored data and returned items. Use a watchdog/interruption path for runaway backend JS and define what happens to all requests owned by an interrupted engine. `setInterrupted` can help stop JS execution; it is not a general process sandbox or a safe way to kill arbitrary native work. [R1]

## 5. Source identity, storage and the provider contract

Keep distinct identities for installed module, module release, account, persistent configured source, browsing selection, backend item, playable variant and playback session. Accounts and servers need not be one-to-one. A single Stremio module may manage several add-ons/configurations, while Plex may expose several servers through one login.

Conceptually:

```text
ItemRef    = persistent source ID + opaque backend item ID
VariantRef = ItemRef + opaque variant/file/version ID
```

The implementation may represent these differently. What matters is retaining origin through details, artwork, caches, search groups, queues, reports, favourites and open UI actions. Host-created handles must be scoped to the caller; do not trust a provider-supplied source string as authority to read another provider's credentials.

Store source IDs independently of tokens and URLs. Migrate existing accounts, settings, cache namespaces and resume state deliberately. Persist authoritative state separately from disposable caches. Providers backed by server state use that state; providers without it can use generic durable local watched/favourite/progress services. A heuristic title match is not permission to copy watch state across sources.

Make request credentials resource-specific. Artwork, video and subtitles can be on different hosts. Scope tokens, TLS exceptions, cookies and redirect behaviour correctly; never apply the currently browsed source's global header to every request. Return opaque/native artwork and playback descriptors so render-time URL construction does not call JS.

### Small base, separately versioned features

Start with an experimental **0.x base API**, independent of Spool and provider release versions. A sensible policy is that `0.1.x` fixes do not break `0.1` consumers, while a breaking draft changes the compatibility revision to `0.2`. Document the exact rule instead of relying on loose semver assumptions. Initially load only explicitly supported draft revisions; do not implement historical-version adapters.

The base covers module/source lifecycle, async operation semantics, error shapes and extension discovery. Keep capabilities separate and granular: catalog/browse, search, details, variants/playback, authentication, artwork descriptors, reports, segments, user state, backend group/remote control, and UI hosting as needed. “Catalog” must not imply support for every Jellyfin query. Stream-only and catalog-only sources must be representable.

Version extensions independently, including the UI-host bridge and any public UI-kit/module dependencies. Distinguish interfaces a provider **offers**, host services it **requires**, and optional services with documented fallbacks. Capability flags may remain convenient UI views, but are not the compatibility protocol.

Unknown offered extensions are ignored. Missing optional host extensions disable only the dependent feature when a valid fallback exists. Missing required features reject activation with an explanation. Make optional dependencies optional during module evaluation and QML loading too: no unconditional top-level import or initializer that fails before negotiation. Load dependent QML components only after their requirements are satisfied. Do not silently ignore unknown permissions, manifest formats or security requirements.

Keep source/extension negotiation language-neutral internally, but implement only the JS adapter now. Do not design a C ABI, ship a Wasm runtime or promise future native binary compatibility. After real use stabilises the shape, an API 1.0 decision can be made separately.

## 6. Concurrent browsing, federated search and version selection

A browsing source controls that view only. Search all enabled/search-capable sources asynchronously, with bounded concurrency and incremental delivery. Multiple instances of the same provider must work simultaneously. Providers search their own sources; core orchestrates the federation.

Use a page contract that can express opaque continuation, unknown total count, exhaustion, supported filters/sort orders, partial errors and provenance. Do not impose offset pagination on a cursor API or simulate unsupported sorting by returning a misleading first page. Query changes invalidate relevant caches/cursors. Any local index or fallback filtering is core-managed and explicitly scoped.

Process each source/page independently, avoid all-or-nothing waits, show unavailable/unsupported sources honestly, and use fair “load more” scheduling. A slow source must not hold up the first results. Preserve focus/selection and scroll position while adding or merging rows. Keep stable result-group identities; do not reset the entire model or continuously reshuffle it as replies arrive.

### Grouping policy

Preserve namespaced external IDs with entity type and provenance; do not require TMDb as a universal primary key. Prefer non-contradictory shared IDs. Missing cross-database mappings may be cached/resolved lazily, but global search must not depend on a third-party metadata request for every hit.

When usable matching IDs are absent, provisionally group **conservatively normalised title + exact year + media type**. Preserve sequel numbers and meaningful title differences. Missing years, fuzzy matches and ±1-year tolerance are not automatic merge rules. Known conflicting IDs prevent a merge, and an unidentified item must not bridge two conflicting groups. Allow later metadata to split provisional groups safely.

Group series separately from episodes. Episode matching needs compatible parent identity and ordering; edition/file/timeline identity is stricter than title identity. Never infer identical cuts, resume positions or watch-party timelines from a shared film ID alone.

Groups retain every source-specific item/version. Selecting a group opens an incremental chooser with provider, server/source or add-on label, edition and available quality metadata. Show resolution, video/HDR/audio details, bitrate and size where known. Distinguish reported/inferred values from verified ones. The optional filename setting is **off by default** and shows a basename, not private paths or token-bearing URLs.

Discover detailed variants lazily; do not probe video files or resolve streams for every search hit. A catalog match is not confirmed availability. On title selection, query applicable source availability by identity even when another source's text search missed the title, without inventing support for APIs that cannot do that.

Pass the **exact selected variant** into resolution. A bitrate/quality preference is not a substitute: resolution must not silently choose another server, edition or file. Carry any preparation/interaction state explicitly, return cancellation/failure honestly, and invalidate short-lived stream descriptors appropriately.

## 7. Provider-owned QML without moving heavy work onto the UI thread

Providers may supply complete pages, dialogs, settings flows, actions, panels and context-menu contributions—not only templates or a fixed set of forms. They can build sophisticated interfaces from native Qt Quick elements and the public Spool UI kit. Keep provider-specific layout, service terminology and workflows in the provider repository. Qt supports custom QML component types and dynamic component creation. [R4, R5]

Provide a small UI-host extension for mounting routes/components, supplying source/item context, opening actions, returning async results, native list/tree models, navigation/focus, localisation and notifications. Let providers compose their own controls; new rendering primitives can be added to the reusable host when genuinely required. Do not expose arbitrary private shell objects as the public API.

Provider UI runs on the GUI-side QML engine. Backend API logic remains on the worker. A UI action invokes the source-bound async bridge and consumes model updates or small result values; it must not obtain direct access to worker-engine JS objects. Large candidate/file lists use native models and virtualised views. Lightweight selection/binding logic is fine; network response parsing, bulk filtering and sorting are not.

A generic interaction flow should support:

```text
A core action requires provider interaction
    -> mount provider-owned QML with scoped context
    -> provider displays native-backed options/progress
    -> return an exact variant/action result, or cancellation
    -> core continues generic playback/navigation
```

For Stremio this permits add-on configuration, release filters, manual source/file choices and preparation status without hardcoding a torrent picker in core. The generic cross-source quality chooser can offer an advanced provider action rather than duplicate the whole detail page.

Specify Back handling, focus restoration, keyboard/remote/touch accessibility, component lifetime, cancellation on dismissal and failure fallback. Use plain text for untrusted titles unless deliberately sanitised. Preload/cache frequently used components where measured useful, and include cold provider UI in benchmarks.

Provider QML uses a declared set of host/Qt modules. Ensure release builds actually ship/register that allowed set: a static import scan of built-in QML cannot be assumed to find imports used only by downloaded providers. Do not import an unsupported module and hope an invisible item avoids evaluation. Component/resource/module names must not collide across providers, source instances or installed versions.

Downloaded archives contain QML/JS **source**, not architecture-specific compiled QML artifacts. Host-generated caches are disposable and keyed to the actual runtime/package; bundled build-time acceleration may use the same source where compatible. A fallback source path must remain tested so installed updates do not depend on a developer's compiled cache.

## 8. Trust, permissions and resource safety

The goal includes third-party providers, but a scripting language is not proof of a security sandbox. Separate a restricted backend API from the broader Qt/QML execution environment. Audit import resolution, native type exposure, dynamic component loading, network/file access, external URL opening and inherited singleton access. Do not claim an engine/context boundary prevents malicious code from hanging the GUI or exhausting process memory.

Initially distribute official and reviewed third-party packages through authenticated catalogues. Represent publisher identity, source provenance and declared access explicitly. Desktop/development builds may support consciously trusted additional repositories/local source directories; do not enable arbitrary unreviewed QML in a store build by default and describe it as safe. This trust decision must not require third-party maintainers to fork Spool or maintain a second provider implementation.

Provide narrowly scoped host services for network origins, source-owned secrets, durable storage, file selection and external navigation. Support user-authorised LAN servers and HTTP where needed; a blanket private-network ban would break the product. Conversely, do not forward arbitrary tokens across redirects or grant a provider access to every configured server. Bind permissions to accounts/source contexts and request explicit approval for newly required access.

Remote API responses, add-on manifests and content descriptions are data—not executable code. Only verified provider source may be evaluated. Reject bundled native libraries, executable installers, QML plugin declarations and undeclared native imports in portable packages. Resolve package imports/resources within verified boundaries and prevent providers from replacing shared host modules. Validate more than filename extensions.

Account tokens and configured add-on URLs can contain secrets in paths as well as headers/query strings. Redact logs, diagnostics, telemetry, crash metadata and screenshots appropriately. Prefer opaque credential handles/injection where practical, but permit a provider to implement its own necessary authentication protocol using only its authorised credentials. No client-side distribution signing private key belongs in the application.

Do not expose a general shell/process launcher, arbitrary filesystem APIs or unrestricted mpv command execution. Add explicit host actions for real needs. Audit network-capable QML types and resource loaders as well as the backend HTTP service; restricting one does not automatically restrict the others.

A malicious or simply broken custom UI can still block a GUI thread. Use review, linting, benchmark tests, limited exposed types and recovery controls as initial protections; stronger isolation is a separate future project. If a required safety/policy property cannot be enforced, narrow the distribution/trust mode and document the limitation rather than pretending that a capability manifest enforces it.

## 9. Portable packages and independent updates

### Format and compatibility

Use **ZIP** for the initial portable provider package. Include a manifest, JS modules, QML source, permitted resources, licence/notices and useful debug/source-map information. Use a maintained safe ZIP implementation; verify its availability and licence rather than assuming that an existing DEFLATE decoder includes archive handling. Do not spend this phase investigating Brotli/xz/zstd size differences.

Illustrative layout, not a mandatory directory API:

```text
manifest.json
logic/provider.mjs
logic/...
ui/SourcePicker.qml
ui/...
resources/...
LICENSE
NOTICE
```

The release is a source package, normally reusable across OS/CPU targets. Actual compatibility still includes the base API revision, extensions, JS feature baseline, allowed QML/host modules and channel permissions. No per-Spool-build native artifact matrix is required. Declare exceptional platform requirements honestly rather than treating all source packages as universally usable.

The manifest/catalogue must identify module ID, independent release version, entry points, base API requirement, offered/required/optional extensions, UI/resource requirements, state schema/migrations, publisher, allowed channels and access requirements. Bind these to exact artifact bytes through authenticated metadata, hashes and sizes. Use Spool-version bounds only for genuine known incompatibilities/features, not as the routine version key.

Do not overspecify a final wire schema before the real providers exercise it. Specify enough to implement a deterministic resolver and unit-test incompatible/missing/optional combinations before independent distribution begins.

### Bundled plus installed

Bundle a pinned, tested Jellyfin package in every initial release. Bundle Stremio and other official providers once ready for that channel. Bundled and downloaded copies use the same provider sources and logical runtime, not parallel implementations.

A compatible, trusted installed update may override the bundled copy at the next startup. Select one version of each provider per process; never initialise both copies or merge their QML files. Store downloaded packages in app-private/user data, not inside the AppImage, signed `.app`, APK or package-managed installation. Keep bundled copies available for recovery, but never activate one whose contract or state schema is incompatible.

Pin installed/loaded versions separately. An update applies to all source instances belonging to that module, preserves their persistent identities and credentials, and does not overwrite code used by existing operations. Prefer restart activation for the first release, despite scripts being easier to reload than native libraries. Qt caches imported modules, and UI/resource lifetimes make casual in-place replacement error-prone. [R1]

### Installation transaction

Resolve a compatible candidate against the current or proposed host and the channel's policy. Download to staging with bounded sizes, authenticate metadata/artifacts, safely extract and validate contents before execution. Defend against traversal, symlinks, duplicate/case-colliding paths and decompression bombs. Do not run provider-supplied installers.

Commit an activation record referencing immutable versioned directories. Keep persistent state outside them, and make migrations recoverable where possible. On restart, initialise the candidate, restore sources and validate basic module/UI health before marking it good. Offline servers or expired logins are not evidence that the module code is corrupt.

On failure, use a compatible known-good version or start a recovery UI with that provider disabled. Never roll an older package back onto a schema it cannot read, or restore an explicitly revoked artifact. Remove code and source/account data as separate user actions.

Use an authenticated update design with key rotation, freshness/replay handling and revocation. TUF is a useful reference; reuse maintained cryptography/update primitives rather than inventing them. Temporary catalogue outages must not disable already verified functioning providers. [R11]

### Release/update coordination

| Situation | Behaviour |
| --- | --- |
| Provider fix supports installed host | Update independently; other providers and Spool do not need a release. |
| Latest provider requires newer host capability | Retain/offer the newest compatible version, not the latest incompatible one. |
| Spool update preserves provider requirements | Reuse existing provider ZIPs unchanged. |
| A draft API break requires new packages | Publish compatible artifacts before advertising the transition; bundled packages are tested together. |
| A third-party provider is not ready | Do not gate the entire ecosystem. Defer an app-managed incompatible transition for the affected user or explicitly update with the provider disabled and state retained. |
| Store/manual/package-manager host update arrives first | The app still starts. Revalidate packages, disable incompatible ones safely, retain accounts and offer recovery. |
| Mandatory security update conflicts with a provider | Surface the issue and allow updating securely without that provider; never silently load incompatible/revoked code. |

Publish immutable packages first, verify they are reachable, then publish signed catalogue metadata. Preserve compatible older releases for supported draft runtimes. Catalogue absence alone is not evidence that an installed trusted version must be deleted.

Provide a manager for installed/bundled versions, enable/disable, updates, compatibility explanations, publisher trust and recovery. Keep **installing a provider module**, **adding an account/server** and **configuring a remote Stremio add-on** distinct in the UI, even where a guided setup combines them.

## 10. Platform and store delivery

The default is the same portable provider architecture everywhere. Distribution policy is a host/channel concern, not separate provider source code. Do not carry forward native shared-library restrictions as though JS/QML source archives were `.so` files.

| Target/channel | Implementation direction |
| --- | --- |
| Windows, ordinary Linux and direct macOS distribution | Bundle official packages; verify/download source ZIPs to user storage and activate on restart. Retain existing host signing/packaging. |
| Linux AppImage on NixOS | Use the same Linux app/provider packages through `appimage-run`; verify writable data paths, imports and cache behaviour. No Nix-specific provider build or `nix-ld` project. |
| webOS Developer Mode, including unrooted | Test downloaded JS/QML source loading from the real app-data directory first. Native executable mappings are no longer a provider requirement. Test interpreter-only execution, QML imports, resources, updates and restart on-device. |
| Android/Android TV outside Play | Same verified source-package path, appropriate app-private storage and host permissions. Do not add native provider download machinery. |
| Android/Android TV on Google Play | Pursue interpreted provider delivery with a constrained, documented host surface; bundle official packages regardless. Play permits an interpreted/VM exception to its code-download restriction, but runtime code must comply with all policies. No native payloads, APK self-updater or alternate native-module installer in this channel. [R8] |
| iOS/tvOS App Store and Mac App Store | Build the same provider runtime and bundled source packages. Seek review/permission for the actual downloaded-provider and native UI/host interface before enabling that path. Until approved, allow multiple bundled first-/third-party providers and configuration of supported remote services, updated through app releases. [R9] |

For webOS, retain the demonstrated app replacement-IPK mechanism as a fallback if actual source/resource deployment constraints require it. A repair/replacement package can include selected provider source packages; no C++ compilation on the television is implied. Test installation state, restart persistence and recovery. Do not implement on-device package composition until a real loading restriction justifies it.

JavaScript removes much native loading/signing/ABI complexity, not every platform limitation. Verify the actual Qt runtime's language features, resource imports, background execution and cold-start behaviour on each target. JIT availability must not be a prerequisite; source packages must work correctly with the supported interpreter path.

Google Play approval is not guaranteed by the word “JavaScript.” Prepare a reviewable explanation of provider purpose, code provenance, capability exposure, content/access policy and update controls. Apple currently imposes additional requirements for downloaded plug-ins, including prior permission for native API/technology exposure, consent and software indexing. Do not assert automatic approval or silently enable a previously unapproved execution path remotely. [R8, R9]

Use the same sources for bundled fallback builds. Store restrictions may limit available packages/features, but must not force authors to maintain a different client implementation or force users into one-provider-per-app forks.

## 11. Implement Jellyfin and Stremio as real clients

### Jellyfin

Port API/auth/session logic to JavaScript; reuse endpoint knowledge, tested behaviours and fixtures from the existing facade rather than preserving C++ dependencies merely for convenience. Move/reuse provider-specific QML in the external repository and adapt it to source-bound proxies and the UI-host API.

Cover login/QuickConnect, discovered and manually configured servers, multiple accounts, libraries, filters/paging, detail/episode navigation, search, resume/next-up, exact media sources, subtitles, direct/transcoded playback, reporting, segments/trickplay descriptors, favourites/played state and existing management/remote features where currently supported. Build an explicit parity checklist from the actual branch; do not declare success after a browse-and-play demo.

Keep transport, decoding, bandwidth measurement and reusable player policy native. Let the provider translate native capability/preferences into Jellyfin requests and results. Translate server event/group messages into generic host actions instead of letting core continue parsing Jellyfin envelopes. Login refresh/reconnect/sign-out must affect the intended sources only.

Use the old implementation temporarily for recorded-response differential tests, with deliberate exceptions for corrected bugs. Migrate real persisted accounts without duplicate logins/source identities. Remove obsolete facade/singleton/composition code after parity; do not ship both indefinitely.

### Stremio

Create `spool-stremio` early enough to challenge the contract while it is still 0.x. This is a **Spool provider implementing the Stremio protocol**, not the server-side add-on itself. One module can configure many remote add-ons and compose their resource contributions. Adding a remote add-on URL is configuration, not another executable-provider installation.

Implement manifest validation/configuration, resource/type/ID-prefix matching, supported catalog search/filter/paging, metadata, series/episodes, stream and subtitle discovery, and timeout/error isolation. Stremio explicitly separates these resources and permits add-ons with no catalog, so do not require every configured add-on to behave like a complete Jellyfin server. [R12, R13]

Begin with documented, authorised HTTP-stream flows and synthetic/local fixtures. Preserve opaque IDs; map recognised external IDs without assuming every add-on uses IMDb. Search catalogues first, then discover streams for selected/focused titles. Expose provenance and distinguish a metadata hit from confirmed playable versions.

The provider owns its add-on settings, candidate selection and optional manual file/source workflow. Request native-backed models and return exact selected variants. Support per-resource headers/subtitles, expiry and useful next-episode hints where the protocol supplies them. Treat inferred quality conservatively. The protocol includes torrent references as well as URLs; do not present an info hash as an immediately playable HTTP resource. [R14]

Actual peer-to-peer transfer is a separate transport capability, not JS metadata work. Where no approved native/external transport exists, state the limitation and offer supported sources. The implementer may add a necessary bounded integration when justified, but must not quietly grow a torrent engine in JS or claim complete Stremio source support from URL-only tests.

Use generic durable local user-state services where no server is authoritative. Keep user-authorised service credentials and configured URLs private. Emby/Plex implementations are future work unless useful to validate a specific abstraction; design room for their identities, accounts, versions and pagination without creating speculative full SDKs for them now.

## 12. Repository creation, SDK and release tooling

Keep core, app, native services, runtime, public UI kit, tests, SDK documentation and packaging in `spool`. Create **`spool-player/spool-jellyfin`** and **`spool-player/spool-stremio`**, using the organisation shown in the supplied commit links after verifying the implementation environment's remotes and access. Do not create `spool-common`, a separate core repository or a reciprocal submodule graph.

During implementation, use `gh` to inspect authentication, repository existence, visibility and permissions. Reuse existing repositories; a permissions/network error is not proof that a repository is absent. Create repositories only under the intended owner. Match the main project's visibility and preserve the applicable licence, attribution and notices. Scrub fixtures/history for secrets before publishing.

Example commands for prepared local repositories, **only after confirming that new public repositories are appropriate and absent**:

```sh
gh auth status
gh repo view spool-player/spool --json nameWithOwner,visibility,viewerPermission

# Inspect/reuse these if they already exist before running creation commands.
gh repo view spool-player/spool-jellyfin --json nameWithOwner,visibility,viewerPermission
gh repo view spool-player/spool-stremio --json nameWithOwner,visibility,viewerPermission

# Run creation individually after resolving the preconditions above.
gh repo create spool-player/spool-jellyfin --public \
  --source ../spool-jellyfin --remote origin --push \
  --description "Official portable Jellyfin provider for Spool"
gh repo create spool-player/spool-stremio --public \
  --source ../spool-stremio --remote origin --push \
  --description "Official portable Stremio protocol provider for Spool"
```

These commands illustrate GitHub CLI's documented existing-source workflow; they have not been executed by this plan. Initialise/commit the intended local content first and inspect existing remotes before using `--remote origin`. Preserve useful history where straightforward, but do not spend disproportionate effort history-filtering a rewrite. Record source commits and attribution. [R10]

Each provider repository should contain its own logic, UI, manifest, fixtures/tests, contract requirements, documentation and source-package release workflow. No provider-specific login/torrent UI remains in Spool simply because extracting it was inconvenient.

Publish lightweight SDK/type declarations, a local runner, contract tests and package-validation tooling from Spool. Provider CI consumes a pinned SDK/tooling version, not a nested checkout of the entire application with its own provider pins. Reuse workflows or a small distributable tool rather than creating a heavy package ecosystem.

Spool integration pins exact provider source revisions or immutable ZIP digests in a reproducible lock mechanism. A one-way submodule is acceptable if it fits current tooling, but not mandatory. Local development must support sibling-repository/source overrides and testing an unreleased host plus unreleased providers together without tagging every edit.

Provider CI validates manifests, JS runtime compatibility, optional-extension fallbacks, QML imports/lint, recorded responses and packaging. It builds one portable release archive, not one native artifact for each OS/CPU. Host CI tests that package on representative runtimes/channels. A Node-based test is useful only as a supplement; the real Qt JS engine must execute contract tests.

Publish checked artifacts before catalogue updates, retain provenance and sign releases through secure CI credentials. Keep untrusted fork workflows away from signing/release secrets. Main builds bundle pinned verified packages and do not fetch mutable “latest” releases. GitHub workflow definitions stay in `.github/workflows`; shared scripts may live elsewhere.

## 13. Provider-agnostic watch-together

Retain this as an explicitly scoped subsequent feature, not a reason to delay the source/runtime foundation. Core owns rooms, clock synchronisation, scheduled play/pause/seek, buffering policy and drift correction. Each participant resolves their own authorised stream through their own provider. Do not share credential-bearing playback URLs or require the same provider/server.

Extract genuinely reusable timing/control machinery from existing SyncPlay code; keep Jellyfin's server/group protocol in its provider. Existing backend-native groups remain an optional capability, distinct from Spool-to-Spool watch-together.

A small Cloudflare Worker plus one Durable Object per room and WebSockets is a reasonable first coordination transport; validate deployment/cost/security details at that milestone. Cloudflare supports coordinating connected clients this way. No STUN server or peer-to-peer transport is required merely to relay state. [R15]

Use short-lived room credentials, bounded messages, rate limits and reconnect/version handling. Relay control state and content identity, not media. Account for mismatched editions/timelines; shared title IDs do not prove synchronisation safety. Require compatible versions or explicit offset/manual matching rather than silently aligning different cuts.

## 14. Delivery sequence and acceptance criteria

Sequence to reduce architectural uncertainty early; adapt parallel work sensibly. Keep usable builds throughout, but do not let an obsolete “one tiny directory move at a time” plan prevent replacing an unsuitable interface. Namespace/layout cleanup is secondary to behaviour and performance.

| Milestone | Required outcome / exit evidence |
| --- | --- |
| **A. Reconcile and baseline** | Inspect actual branch, record reused/replaced areas and existing feature coverage. Establish reproducible native baseline, real minimum-device target, phase timing and frame/memory metrics. Clearly distinguish tests actually run from reported results. |
| **B. Runtime vertical slice** | A small JS fixture performs host HTTP, maps a bounded page off-thread, returns a QCoro-style native result and updates the existing model. Open one provider-owned QML action. Test interpreter mode, cancellation and exceptions. Measure before expanding the abstraction. |
| **C. Identity and concurrency** | Two source instances of one module and a different module work together, with overlapping local IDs. Browse B while playing A and searching A/B/C; no token/state/callback confusion. Source removal and module failure are contained. |
| **D. Real clients and repositories** | Use `gh` to create/reuse the two repos. Implement a Jellyfin slice and a real Stremio catalog-to-variants-to-playback slice early, then complete the Jellyfin parity/migration checklist. No provider-specific route/global required in shared UI. |
| **E. Federation and custom flows** | Progressive cross-source search, conservative grouping, stable focus, lazy variants, explicit exact-version playback and a provider-owned custom selection workflow all work with partial failures. |
| **F. Independent delivery** | Signed source ZIPs, manager UI, bundled fallback, compatibility resolution, restart activation and recovery work end-to-end. A provider-only release updates two of its configured sources without rebuilding the host. An unchanged compatible ZIP survives a host update. |
| **G. Target/channel validation** | Measure real unrooted webOS loading/performance and Android/TV behaviour, including source-loaded QML. Prepare Google Play review material. Validate Windows/Linux/macOS and AppImage-on-NixOS. Keep Apple downloaded-code activation conditional on review; bundled mode must work. |
| **H. Consolidate and document** | Remove obsolete native provider implementation/plans, export tested SDK/examples, document supported 0.x revisions and channel policies, retain regressions/benchmarks in CI and make release behaviour reproducible. Do not label the contract stable 1.0 automatically. |

Watch-together follows as its own delivery milestone, using the identity/playback foundations above. New production Plex/Emby providers, native plugin ABI support, Wasm, native Nix distribution, and permanent old-API adapters are not prerequisites.

### Tests that must exist

**Runtime and threading:** real Qt JS execution; source factories without global account state; async completion exactly once; network/script errors; timeout/cancellation; engine shutdown; late replies; model-thread correctness; runaway backend JS recovery; repeated install/use/remove without growing retained heaps. No backend JS invocation during native model role reads or frame rendering.

**Data and federation:** pagination/cursors/unknown counts; unsupported filters; out-of-order and failed sources; immediate query invalidation; overlapping IDs; external-ID matches/conflicts; conservative title/year grouping; unidentified-item bridge prevention; episode ordering; different editions; late merge/split; stable remote focus; exact selected file resolution; source-specific credentials for artwork/video/subtitles.

**UI and extensibility:** non-Jellyfin auth page; custom large virtualised candidate/file list; actions returning asynchronously/cancelled; missing optional extension/module remains usable; required extension rejected; imports outside policy denied; multiple UI instances; translated text, long labels and remote/touch/keyboard behaviour. Successful module loading alone is not sufficient.

**Updates and security:** invalid signatures/hashes; unsafe ZIPs; oversized decompressed content; wrong identity/API/UI requirements; unknown required permissions; stale metadata; catalogue outage; failed migration; interrupted activation; incompatible externally updated host; bundled fallback with incompatible schema; revoked-version handling; permissions expanded by an update; secrets absent from logs.

**Performance:** cold and warm provider activation; cached and uncached-data pages; normal and worst-case payloads; simultaneous search completions while scrolling; provider custom UI under load; high-bitrate playback concurrent with browsing; interpreter-only weakest-device runs; first-useful-frame and frame-tail metrics, not only average FPS or network-inclusive averages.

Use controlled LAN/test services, recorded authorised responses and local fixtures. Do not depend on random public streaming add-ons or real private accounts to make CI deterministic. Add a build/test mode with no Jellyfin provider present so hidden shared-UI assumptions cannot return.

## 15. Implementation handoff and completion standard

Begin by reading the branch and implementing the measured JS-provider vertical slice; avoid a large up-front framework rewrite. Keep the public draft narrow while proving real Jellyfin and Stremio requirements. If conversion, page insertion, component creation or another stage dominates, fix that stage using evidence rather than immediately adding Wasm or restoring native provider clients.

At completion, hand over the changed Spool code, both provider repositories, reproducible package/release tooling, contract/author documentation, migration/recovery notes, tests and benchmark results. Record any device/store/access gates that were not actually exercised. Missing credentials or hardware should not prevent completing local implementation and fixtures; do not claim the external step succeeded.

The decisive acceptance scenario is:

> One Spool process uses multiple Jellyfin accounts and multiple configured Stremio add-ons at once; progressively searches them; groups equivalent titles without losing origin; opens a provider-owned selection workflow; plays the exact chosen version through the native player; and keeps scrolling/navigation smooth while sources respond. A verified provider ZIP can update that client logic and UI without rebuilding Spool on an enabled channel, while bundled and store-restricted builds use the same provider sources.

The durable design is **portable provider logic, native fast paths, explicit source identity, rich hosted UI and channel-aware delivery**. Everything else in this plan may be simplified or improved when the implementation provides a better answer.

---

## Sources and verification boundaries

Architecture and sequencing above are proposed decisions reflecting this discussion, not claims imposed by the external references. Performance numbers are provisional budgets, not measured results. External references were checked on 21 September 2026; recheck store policies and the actual pinned Qt/SDK versions when shipping.

### Supplied project material

- **[S1]** `Pasted markdown(20260920-182440).md`: complete supplied commit export, including the commit IDs in section 3 and links under `spool-player/spool`. It documents claimed branch changes, not independent test results.
- **[S2]** `spool-refactor-provider-seam.zip`: inspected source snapshot, notably `src/provider/`, `src/media/MediaTypes.h`, `src/app/`, `qml/shell/RouteStack.qml`, provider-specific QML, `src/diagnostics/RenderBenchmark.*`, tests and CMake. The implementing checkout may have advanced.
- **[S3]** `Spool Core Split Plan(1) (1).md` and `docs/provider-split-plan.md` in the snapshot: earlier native-provider proposal and branch status. This document supersedes conflicting native-plugin, single-active-provider, lockstep-build and exact Spool-version assumptions. Earlier review documents remain inspection aids, not controlling requirements.

### Primary technical and policy references

- **[R1]** [Qt: QJSEngine](https://doc.qt.io/qt-6/qjsengine.html) — module loading/caching, script/native calls, ownership and interruption.
- **[R2]** [Qt Quick: Performance considerations](https://doc.qt.io/qt-6/qtquick-performance.html) — asynchronous work, worker processing, conversions, models and delegates.
- **[R3]** [Qt: QAbstractItemModel](https://doc.qt.io/qt-6/qabstractitemmodel.html) — model-thread requirements and incremental updates.
- **[R4]** [Qt: Defining object types through QML documents](https://doc.qt.io/qt-6/qtqml-documents-definetypes.html).
- **[R5]** [Qt: Dynamic QML object creation](https://doc.qt.io/qt-6/qtqml-javascript-dynamicobjectcreation.html).
- **[R6]** [Qt: JavaScript host environment](https://doc.qt.io/qt-6/qtqml-javascript-hostenvironment.html) — verify against the exact deployed engine, not browser assumptions.
- **[R7]** [QCoro: QmlTask](https://qcoro.dev/reference/qml/qmltask/) — optional task presentation to QML, not a ready-made backend JS bridge.
- **[R8]** [Google Play: Device and Network Abuse](https://support.google.com/googleplay/android-developer/answer/9888379?hl=en) — native download restrictions and interpreted-code exception/conditions.
- **[R9]** [Apple: App Review Guidelines](https://developer.apple.com/app-store/review/guidelines/) — particularly downloaded code, section 4.7 and native technology exposure.
- **[R10]** [GitHub CLI: gh repo create](https://cli.github.com/manual/gh_repo_create).
- **[R11]** [The Update Framework specification](https://theupdateframework.github.io/specification/latest/) — authenticated updates, metadata freshness and trust lifecycle.
- **[R12]** [Stremio add-on protocol](https://github.com/Stremio/stremio-addon-sdk/blob/master/docs/protocol.md).
- **[R13]** [Stremio manifest format](https://github.com/Stremio/stremio-addon-sdk/blob/master/docs/api/responses/manifest.md).
- **[R14]** [Stremio stream response format](https://github.com/Stremio/stremio-addon-sdk/blob/master/docs/api/responses/stream.md).
- **[R15]** [Cloudflare Durable Objects: WebSockets](https://developers.cloudflare.com/durable-objects/best-practices/websockets/).
