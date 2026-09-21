# Spool portable providers: remaining implementation and A/B handoff

**22 September 2026 · implementation brief, not a completion claim**

## 1. Objective and authority

The next useful milestone is a build in which the user can select **native Jellyfin or portable JavaScript Jellyfin**, browse the same real library, search, open details and play a selected version, then compare responsiveness. Do not make that milestone wait for the whole provider ecosystem, updater or Stremio implementation.

The final architecture remains one ruthlessly fast native Qt/libmpv application with multiple first- and third-party JS/QML providers and multiple accounts/servers usable concurrently. Provider authors publish **one source ZIP per release**, not a native platform/CPU build matrix. Official Jellyfin is bundled; independently installed compatible packages can override bundled packages on supported channels. Provider implementation, account, source, item and version identities are different things.

The implementer may change interfaces, directory layout and sequencing when code or measurements justify it. Prefer a simple correct implementation over speculative frameworks. Rewrites are cheap at this stage: reuse working behaviour, tests, the native player/rendering/model infrastructure and useful dependency boundaries, not an unsuitable implementation merely because it exists. Preserve user credentials and authoritative state; disposable cache schemas may be reset deliberately.

### Non-negotiable performance boundary

```
native controller / QCoro call
  -> provider operation on its worker-owned QJSEngine
  -> native asynchronous HTTP
  -> worker-side JSON parsing and provider normalization
  -> bounded conversion into native-owned page data
  -> source/generation validation
  -> bounded GUI-thread model updates
  -> existing Qt Quick delegates and native scene graph
```

No backend-provider invocation from model-role reads, layout, scrolling or per-frame rendering. No worker QJSValue or QObject crosses into the UI engine. Small QML handlers may run on the GUI thread; backend calls, substantial transforms and broad result processing must not. Do not insert stringify/parse round trips between JS normalization and native data. Repeatedly visiting a cached page should bypass the provider when its freshness policy permits.

Target the first useful page frame in **under 100 ms when the necessary response/cached data is available** on the minimum supported webOS device. Measure input-to-useful-frame separately, including network time. Preserve smooth animation while work is pending. A 100-ms transition allowance is not permission to block the GUI for 100 ms. Start with a provisional approximately 1–2 ms budget for an individual GUI-side model commit, then validate it on hardware; a fixed number of rows is not a time guarantee.

No minimum webOS model/firmware is actually established in the supplied materials. Use available hardware for labelled provisional results and settle the supported minimum explicitly before claiming acceptance. Do not silently replace “minimum webOS” with a developer desktop or an assumed C2.

## 2. Baseline and this handoff

The uploaded Git repository is on `refactor/provider-seam` at:

```
c1cb0652a9d72ae954602482511f3d147e4f37c9
```

The accompanying session report is `PORTABLE-PROVIDER-SESSION-20260921.md`. It reports 85/85 tests passing **before this handoff**. It explicitly says normal application browsing/playback still uses native Jellyfin. Stremio, the application cutover, independent package installation and device validation are not done.

Two local implementation commits have been added on `review/provider-ab-handoff`:

| Commit | Change | Important limit |
| --- | --- | --- |
| `215f67b` | Start route timing before synchronous construction; correctly classify cache hits; avoid double activation on synchronous promotion; use a cancellable benchmark deadline; record runtime/environment/failure metadata; reject unlike/incomplete report comparisons. | Timing schema 2 needs fresh baselines. Actual Qt rendering and full app compilation were not run here. |
| `c21f977` | Add worker-side, directly decoded native media pages with source identity, external IDs, opaque cursor, optional total and exact ticks. Expose `callMediaPage` and generation-guarded `callSourceMediaPage`; share cancellation/watchdog handling with small RPCs; add tests. | Listing foundation only. Normal app wiring remains native; these new C++ changes require compilation and Qt tests. |

The documentation commit containing this plan follows those commits. No remote was pushed, no provider repository was modified, and no machine/TV belonging to the user was contacted. The original untracked `spool-portable-provider-implementation-plan.md` was left unchanged and uncommitted.

### Verification in this environment

Passed:

- `tests/tools/RenderBenchmarkReportTest.py` against the updated comparison tool, including incomplete-run rejection and mismatched timing/environment cases.
- Ten `tests/tools/provider_package_test.py` tests.
- `tools/check-module-seam.sh --strict`.
- Five deterministic scenarios executing the actual `RouteStack.showRoute`/`handleLoaded` functions with JS loader/clock doubles under Node. The original function fails the cold-miss regression; the new function passes. This is a function-ordering test, not a QML rendering benchmark.
- Syntax check of the changed JS fixture; the existing bundled Jellyfin protocol smoke fixture under Node with only its import URL redirected to the bundled source. This is not verification in Qt's engine.
- `git diff --check`.

Not run: the new C++ tests, the full Qt suite, actual application A/B tests, renderer timing, physical TV tests, store tests. CMake stops at Qt discovery because this environment has no Qt 6.11 development installation or Nix. Do not lower the project's Qt requirement just to accommodate this environment. The supplied handoff archive includes the configure failure log and patch application instructions.

## 3. What the uploaded benchmark actually measures

Source reports: `provider-baseline-warm.json`, `provider-baseline-cold.json`, `provider-baseline-summary.json` and the session report. The raw files are preserved separately in the handoff archive.

- Linux desktop, offscreen Qt Quick **software adaptation**, native local-provider route workload.
- Five measured iterations, 30 transitions each. The route script is `home -> search -> settings -> home -> libraryGrid -> openSourceNotices -> home`; already-current routes are skipped.
- No named library (`library` is empty), no scroll samples, no submitted search query in this script. Opening a search page is not executing a backend search.
- All warm samples report zero instrumented delegate creations. Every cold sample reports `cacheHit: hit` and a `:warm` name. Neither observation establishes a populated media-view workload.
- The harness requests updates every **8 ms**, irrespective of the nominal 16.667-ms frame budget. That is not a physical 60-Hz display measurement.
- The “cold” option calls application critical-memory-pressure eviction before transitions. It is not a fresh process or a flush of every OS, Qt disk or AOT cache.

Nearest-rank results, recomputed from the supplied raw samples:

| Route | Warm wall median / p95 (ms) | Eviction wall median / p95 (ms) | Eviction GUI CPU median (ms) |
| --- | ---: | ---: | ---: |
| Home | 6.754 / 7.266 | 20.515 / 28.374 | 4.971 |
| Library grid | 6.115 / 6.432 | 4.116 / 4.743 | 2.544 |
| Open-source notices | 6.409 / 6.695 | 5.270 / 5.683 | 3.716 |
| Search page | 6.717 / 6.789 | 3.106 / 3.633 | 2.102 |
| Settings | 6.729 / 7.149 | 28.919 / 34.278 | 9.044 |

Home has ten samples; each other route has five. The pooled p95 values (7.149 and 29.205 ms) are not per-route guarantees or robust tail estimates from a large experiment.

### Measurement defects found in the uploaded source

`qml/shell/RouteStack.qml::showRoute()` called synchronous `loaderFor()` and potentially completed a promoted loader **before** starting `InputLatency.beginUiTransition()`. It then classified the newly created ready loader as a cache hit. Construction could therefore be excluded from both the wall and GUI CPU sample. This is reproduced by the function-level regression test, not merely guessed from the labels.

The first handoff commit moves classification and timing before construction. It preserves the foreground loading policy rather than mixing measurement repair with a speculative UI performance rewrite. A promoted loader can emit `onLoaded` synchronously, so the new pending-loader guard also prevents double activation.

The benchmark also left a separate uncancelled timeout for each step. A successful earlier step's timeout could interrupt a later awaiting step. It now uses one owned deadline timer, records failures and fails incomplete runs. New metadata records the timing origin, QPA platform, Qt/ABI, renderer API, dimensions, requested interpreter state, frame-pump interval and actual native provider identity. A new `requestToSampleMs` stopwatch offers a cross-check around the route request.

### Interpretation

These numbers do **not** measure an added JS-provider cost. They provide no basis for claiming an extra two frames caused by JS. The approximately 8–10 ms of measured GUI CPU in the settings eviction samples is worth investigating on weaker hardware, especially since some construction was omitted. That is a pre-existing/cross-cutting UI concern, not a reason to revert the provider language decision.

`presentMs` includes time after content readiness until Qt's observed frame callback; it is not panel latency. `actualSwaps` counts Qt callbacks. `budgetIntervals` rounds total time into nominal intervals; it is not dropped frames. `maxGapMs` is periodic timer lateness, not continuous GUI occupancy. Zero lateness in a short sample does not prove there was no blocking. GUI CPU is cumulative thread CPU across the measured interval, not necessarily one uninterrupted function.

Do not scale desktop wall time by an invented TV CPU multiplier. Do not treat the software adaptation as Mesa llvmpipe or as the TV's GPU path. Qt documents the separate software adaptation and the semantics of frame callbacks [R1–R3].

## 4. Immediate work: obtain a trustworthy test build

### Step A — validate the local commits and keep a native control

1. Apply the handoff commits on the existing branch without rewriting its history. Preserve unrelated dirty/untracked work. There is no need to recreate the main repo or the Jellyfin repo.
2. Build with the project's pinned toolchain. The supported full entry point is `nix run .#tests`; targeted runs can use the generated build directory from that workflow. Run at least `route-transition-timing`, `provider-media-page`, `provider-media-page-interpreter`, `script-runtime`, `source-registry`, `provider-ui-context`, the bundled Jellyfin contracts and report/package tests, then the regular suite.
3. Inspect and fix any compilation/behaviour defects in the new C++ code. It was deliberately delivered with tests and an explicit uncompiled status, not presented as a green release.
4. Check the shared sink refactor with both ordinary `QVariantMap` calls and typed page calls. Validate cancellation, shutdown, invalid results and watchdog failure, not only the happy path.
5. Run the normal native application with no new selection flag and confirm unchanged user behaviour. Do not call it the JS build just because a script module was registered or warmed.
6. Re-record native route baselines using timing schema 2. Keep renderer, scale, window size, viewport and cache mode consistent. Do not compare old post-construction totals with corrected totals as a regression.

Avoid spending this phase on renaming every namespace, extracting another shared repo, changing archive compression or implementing backwards compatibility. The API remains experimental 0.x; extensions are separately versioned.

### Step B — wire the first real JS catalog/search path

Use the existing provider interfaces and native models where appropriate. Add a portable/source-bound adapter that invokes actual bundled Jellyfin operations through the registry and returns typed values to the controller. `callSourceMediaPage()` now supplies the expensive worker-side part; it is not yet a `Catalog` or `SearchSource` implementation.

The new `ProviderMediaPage` is intentionally a listing envelope. Each item retains `sourceId`, its current `MovieItem` fields and normalized external IDs. Keep that source identity through navigation, caches and subsequent operations; do not discard it to fit an old global-source assumption. Extend/refactor native models if necessary. Do not quietly route details/artwork through whichever account is selected later.

Map real operations: libraries, bounded filtered/sorted browse, paged search, resume, next-up, latest, seasons/episodes, similar and details. First make a narrow real vertical slice work, then add the rest. New typed decoders are appropriate for large details/variant payloads; small authentication/settings operations may keep the bounded generic RPC path. Do not relocate bulk result conversion to GUI coroutine continuations.

Resolve pagination deliberately. The old `Catalog` API takes numeric offsets; portable pages expose opaque cursors and possibly unknown totals. A temporary Jellyfin-only adapter may translate its known offset protocol, but core must not assume every provider cursor is an integer. Retain per-query cursor/exhaustion state; reject non-advancing repeated cursors or bound recovery. Do not treat an empty filtered page as final when it advertises a valid advancing cursor. Preserve cancellation and request-generation checks before committing rows.

**Prevent false performance wins:** validate row counts, ordering, metadata used by delegates, artwork and page bounds against the native path. Returning fewer fields/items or an empty result is not an optimization unless the same product behaviour is preserved and the workload difference is explicitly measured.

### Step C — bootstrap a real account, then bind resources correctly

For the first A/B build, reuse the user's existing native saved-account/login UI as a temporary bootstrap into the portable source registry. Do not block that test on completing all provider-owned login screens. Clearly label this temporary mixed bootstrap; it must not proxy catalog/search/playback requests to the native facade.

Resolve the saved profile to stable host-created account/source identity. Supply the current server/user/token/device/locale configuration and explicit allowed origins to the JS source factory. Reconfiguration must not generate new source IDs or overwrite the credentials of another account. Never put tokens in CLI arguments, benchmark outputs, public metadata or source IDs.

Adopt source/resource-specific artwork and playback descriptors. Existing artwork handling still has global-auth assumptions. The UI must not show a real library by silently using a native global credential singleton. Image, subtitle and video headers/trust context can differ and must follow their resource. Keep native downloads, decoding, caching and GPU upload.

Integrate the existing TLS trust policy with worker networking; self-signed/local-server handling must not become blanket `ignoreSslErrors()`. Unauthorized responses must produce a useful source-specific state rather than wiping unrelated results. Do not invent missing storage or credential-service support because the manifest names a permission.

### Step D — add a genuine backend selector and enough playback

Introduce a temporary explicit native/portable-JS Jellyfin selector (CLI/environment or developer setting; choose the smallest suitable surface). No flag currently exists for this. Make diagnostics report the **effective** backend, not just the requested value. Reject an unavailable JS selection rather than falling back silently.

Select before composition and keep the same core UI, fonts, model policy and playback implementation. Ensure only the chosen path submits catalog requests and playback reports. Reusing a native session bootstrap must not instantiate duplicate progress reporting or protocol timers. Delayed QML warming, disk caches and diagnostics settings must be matched between comparison runs.

Wire basic details, variant discovery, exact variant selection, direct playback and start/progress/stop reporting. Check seek/resume, audio/subtitles, error handling and the currently selected version. Follow-up work extends transcoding/device profiles and the remaining parity matrix, but a variant must never silently become a different edition/file.

### First user-testable milestone: acceptance

- User can explicitly choose native or JS Jellyfin in the same build.
- Actual source provenance shows JS calls for the operations under test.
- A real account browses and paginates a populated library, searches, opens details and plays the chosen version.
- Counts, visible metadata, artwork, identity and basic state/reporting are correct.
- Cancellation/back-navigation cannot publish stale results or corrupt selection.
- Scrolling never calls the backend script for existing row data.
- Repeated/cold page transitions can be measured on identical build/runtime conditions.
- Known unsupported features are visible in the handoff; there is no silent native fallback.

Do not claim this milestone merely because all unit tests pass or local-fixture navigation is fast. Keep the native implementation until the comparison and parity work have actually justified removing it.

## 5. Performance measurement that answers the real question

Use three complementary comparisons:

1. **Matched native versus JS in the same current build** to attribute provider overhead without confusing it with unrelated UI changes.
2. **Current app versus the actual pre-split executable** for total user-visible regression. Apply equivalent instrumentation to a throwaway baseline build or use a common external observation method; do not compare different timer origins.
3. **Controlled response replay/loopback fixture** to isolate processing from server/network variation, followed by a real server on LAN and target-device tests.

Record build/provider revision and package hash, effective backend, Qt version, interpreter request, actual renderer/QPA, viewport/scale, device identifier chosen for diagnostics, source count, page limit, payload bytes, result count, cache state, warmup and samples. Do not log response contents, tokens or credential-bearing URLs. If a metadata field is merely a user label rather than measured fact, say so.

Add optional request tracing with a host-generated operation ID. Useful boundaries: controller dispatch; worker dequeue; HTTP start/response complete; JS normalization complete; native conversion complete; future received; first GUI batch committed; first useful viewport/frame callback. HTTP waiting, queueing, conversion and rendering must not be collapsed into “JS time.” Measure bytes/rows and worker/GUI CPU when available. Keep hot-path tracing disabled or bounded in normal builds; do not add an unbounded event log.

The worker sink creates a natural place to time native conversion. A small measured hook around provider normalization can separate it from transport. Choose practical instrumentation rather than a large tracing framework. Measure memory and GC/tail behaviour across sustained navigation, not only the first tiny page.

Workloads must include:

- Populated movie/TV pages at ordinary sizes (e.g. the existing 72-row default), later pages and filters/sorts; already-normalized cached revisits.
- A search query actually issued to one source and then several sources; responses finishing together and out of order.
- Detail/episode/variant lists, long titles/Unicode, missing metadata, unknown totals and a representative large but bounded response.
- Custom provider selection lists with tens/hundreds of rows and the existing 2,000-candidate fixture; verify virtualization and retained focus.
- Cold process, cold source/engine, evicted page instances, cold artwork and warmed resident pages as **separate labelled cases**.
- High-bitrate playback while browsing/searching, low-memory pressure, a slow/offline source, cancellation and prolonged repeated queries.
- Interpreter-only JS as well as the platform default. JIT must not be necessary to meet the supported profile.

For display pacing, disable the artificial pump (`SPOOL_BENCH_PUMP_MS=0`) where the normal on-screen render loop suffices; record the setting. Offscreen reports remain useful regression diagnostics but never become panel measurements. Report per-route/per-stage distributions as well as pooled summaries. Use enough repeats for meaningful tails and preserve failures rather than dropping slow samples.

If a trace regresses, optimize the measured cause: eliminate unused payloads, avoid repeated normalization, cache native pages, reduce bridge churn, bound concurrency, improve model commits, or fix costly QML construction/bindings. Do not jump to Wasm/native providers before identifying an actual provider execution bottleneck.

**TV safety:** current repository instructions prohibit installing/launching without explicit user permission and prohibit mutating the installed app tree. `tools/webos/bench-device.sh` currently rewrites the installed executable into a wrapper and changes ownership; do not run it as-is. Provide a supported packaged diagnostic/launch path instead, using the platform's actual supported mechanism. Do not assume root is available. No TV action was performed in this session.

## 6. Complete the source-aware application after the A/B slice

Registry-level source identity already exists; application ownership still needs conversion. A selected browsing source only chooses the displayed library. The player, queue, outstanding requests, artwork and state writes retain their own source context independently.

Make source-scoped item/version references first-class in routes, queue entries, cache keys and actions. Source/account removal or disable must cancel the appropriate work and invalidate stale handles, not redirect them. Resolve player features against the playing source and UI actions against their bound source, not a global capability singleton. Run two accounts in one module and another module simultaneously with deliberately colliding backend IDs.

Add persistent account/source management and migrations from existing profiles. Preserve tokens, settings and authoritative watch state. Differentiate disconnect/disable/remove code/remove source/delete data. Background network failure must not erase a valid configured identity or another source's result cache.

Review source/module/operation limits against realistic concurrent use. The current bounded runtime is a useful starting point, not a promise that hardcoding four modules is the final ecosystem policy. Avoid one network burst or provider CPU burst starving native rendering. Keep runtime lifetime management off the interactive hot path; the current runtime destructor waits for its worker and should not be triggered by a routine page transition.

## 7. Complete Jellyfin behaviour and provider-owned UI

The published Jellyfin repo is reported at `spool-player/spool-jellyfin`; the application pins payload revision `673647c83ff64714ee745aac09829fb63db6ee42` and a deterministic 11,291-byte ZIP. A later reported repo commit updates the SDK snapshot without changing the payload. Inspect the actual current repo and local override before altering pins; do not treat every repository commit as a changed provider release.

Use `gh repo view`/`gh repo clone` or an existing sibling checkout when available. `SPOOL_JELLYFIN_SOURCE_DIR` already supports unreleased local integration. Do not force a release/tag/pin bump for every joint edit. Do not edit the vendored ZIP as the authoritative source or silently bypass its digest checks. No remote pushes unless explicitly authorized.

Audit the native parity inventory in `docs/portable-provider-decisions.md` and preserve relevant protocol fixtures. The JS client has endpoint coverage, not proven complete parity. Specific inspection leads from the current payload:

- `item()` normalizes a subset of requested metadata. Album fields, some inherited artwork, dates/status, external URLs and detailed media-source information used by existing screens need auditing; details requests ask for information the mapper may not return.
- `raw.ParentIndexNumber || null` conflates season zero with missing metadata. Preserve zero where meaningful.
- `String(raw.Id)` can turn a missing ID into the literal `undefined`; validate required IDs rather than manufacturing them.
- Full source negotiation, native capability/device-profile construction, stream-quality policy, audio/subtitle selection, segments/trickplay, transcoding cleanup and event-driven updates need real-server coverage.
- SyncPlay/remote functionality needs backend events/WebSockets as well as REST endpoints. Do not describe REST stubs as feature parity.

Port protocol operations and provider-specific controllers to JS/QML while leaving generic native timing, playback and transport algorithms native. Avoid a permanent JS facade that still calls Jellyfin's old C++ endpoints; that defeats independently maintained portable providers.

Move Jellyfin login, linking/profile, management, remote and SyncPlay UI to the provider repository once the public UI-host facilities exist. Provider-specific UI must not require editing shared Spool routes. Preserve navigation tests and behaviour, not fixed globals such as `Session`, `SyncPlay` and `Management` in shared QML.

The existing `ProviderUiContext`/native list bridge is useful. Finish provider route/action descriptors, creation with source context, Back/cancel, focus restoration, page lifetimes, progress/errors, localization and constrained host operations. A provider should own a complete custom file/torrent/source-selection workflow and return an explicit variant or cancellation. Core need not understand that workflow's domain.

Keep core content models native and efficient. The generic custom-list `record` map is a separate convenience surface; profile repeated map conversion for large custom delegates and expose appropriate native roles if necessary. Do not replace all core media models with generic JS arrays because the bridge exists.

## 8. Federated search and exact-version selection

Build a core coordinator over all enabled/searchable source instances. Query concurrently within a fair budget; display each completed page independently, retain per-source cursor/loading/error state and discard stale generations immediately when the query changes. One unavailable server cannot block or clear successful results.

Group **logical titles**, retaining all source-specific items/versions. Prefer compatible external IDs, scoped by entity type. Otherwise provisionally group conservative normalized title + exact year + media type. Known conflicting IDs override heuristics; an ID-less result must not bridge two contradictory identified groups. Keep missing/uncertain matches separate unless additional evidence resolves them.

Do not use grouping as permission to overwrite watched state or resume across copies. Editions can share an identity but differ in timeline. For TV, series identity and episode ordering are separate problems; episode matching needs compatible episode IDs or validated numbering. Keep stable result-group identity, remote focus and menu selection during incremental merges or later splits.

The chooser shows source/server and provider, quality/provenance/edition and optional **basename**, disabled by default. Unknown metadata stays unknown; do not probe every stream to fill a search screen. One source may expose many variants. Propagate the selected `VariantRef` into resolution and fail rather than substitute an unrelated edition.

On selecting a logical title, query applicable sources for availability even if they did not return the original text-search hit, where their protocol/index supports this. Catalog matches and confirmed stream versions are different states. Cross-provider scores are not automatically comparable; begin with a deterministic understandable ranking and avoid disruptive reranking.

## 9. Stremio and future provider validation

Stremio is still missing. It should exercise the generic contract early after the Jellyfin A/B slice, before stabilizing APIs. A minimal real catalog -> details -> stream-list -> chosen stream -> playback flow is more valuable than completing every generic manager first.

Inspect with `gh repo view spool-player/spool-stremio`. If absent, create that independently maintained repo with the intended ownership, matching visibility and appropriate licence using `gh repo create` once remote creation is authorized. Do not recreate `spool-jellyfin`. No nested/mutually recursive application submodules; consume the documented small SDK/test tools and export/pin them reproducibly.

A Spool Stremio provider is not the same thing as each remote Stremio add-on. Implement manifest/resource handling, configurable endpoints, catalog/search pagination, metadata identity, stream discovery, request headers, subtitles and source preparation as supported. Stream-only add-ons can contribute playable versions for an identity found elsewhere; they need not supply a catalog hit.

Provider QML owns service-specific release/file selection and preparation screens. Distinguish an existing stream, a resource awaiting preparation and an unavailable resource. Actual peer-to-peer torrent transport, if later required, is separate native/remote service functionality; do not accidentally add an entire torrent engine to every script client.

Plex/Emby remain future implementations unless the user expands scope. Keep the interface neutral enough for their account/server enumeration, media versions, artwork and reporting. Their future presence should not require a new single-active-backend rewrite.

## 10. Finish the experimental SDK and extension model

Keep base API revisions at 0.x; bump for an actual breaking base-contract change, not each app build. Version feature/UI extensions independently. No historical-API adapters or universal ABI framework now. The new typed native page envelope is host implementation work, not a native provider ABI or a reason for CPU-specific ZIPs.

Separate offered provider capabilities from required/optional host services. Unknown offered extensions can be ignored. Missing optional host extensions require real fallback paths, including loading optional QML only when its imports/types exist. Missing required extensions reject activation. Test absent extensions by using the remaining UI, not only importing a module.

Document the actual supported JS syntax/modules and QML import baseline. Provider factories, per-source state, asynchronous lifetime, cancellation, errors, storage, credentials, resource ownership and UI mounting are the important contracts. Do not expose all concrete core-controller pointers or every private UI component as public API.

The current validator intentionally rejects unavailable public UI modules/extensions. Keep that fail-closed behaviour until those facilities really exist; update the runtime, validator, manifest capabilities and tests together. Prefer a small real interface to a schema that advertises future services.

Retain contract tests in the actual shipped QJSEngine, including interpreter mode. Node can supplement syntax/protocol tests but is not proof of Qt compatibility. Produce one platform-independent source ZIP; platform runtime testing is not a native compilation matrix. Host-generated QML/JS caches are disposable runtime optimizations, not required architecture-specific provider payloads.

## 11. Provider installation, updates and distribution

ZIP packaging/structural validation already exists. Publisher authentication and activation do not. Keep those distinct.

Implement a small catalogue/resolver/downloader/activation path with authenticated metadata, exact hashes/lengths, trusted publishers, key rotation, freshness and revocation. Use a maintained design/cryptographic implementation; do not invent a signing protocol. Existing artifact provenance alone does not establish who the user trusts or prevent stale catalogue replay.

Validate/extract before execution, preserving path/type/size/symlink/case-collision and executable-payload protections. Store immutable versioned packages in user/app-private data, not inside the AppImage, APK or signed application bundle. Keep credentials and authoritative provider state elsewhere. Do not run provider installer scripts.

Select one compatible version per provider per process. Bundle official Jellyfin; let a verified compatible installed release override it on restart. Updates affect all that module's source instances, preserving identity. Keep compatible known-good recovery, but respect state schema changes and revoked releases. An offline server is not evidence that the new module is broken.

The resolver uses base/extension/JS/QML/channel/state requirements, not exact Spool version by default. Publish artifacts first and authenticated catalogue metadata last. Stage compatible provider replacements before app-managed incompatible transitions; external/store updates must still start with incompatible modules disabled and accounts retained. One abandoned third-party provider must not block the entire app ecosystem or essential security fixes.

Expose module installation/enabling/removal separately from account/source configuration and remote add-on setup. Provide useful failure/recovery explanations without exposing credentials. Start with trusted/reviewed packages. Qt JS/QML in-process execution is **not a security sandbox**; static import checks and a narrow facade do not make arbitrary hostile QML safe. Explicitly resolve third-party publisher trust before enabling unrestricted repositories.

### Channel requirements

- Desktop direct distributions: bundled packages plus verified source downloads/overrides; retain native host signing/packaging.
- NixOS: AppImage through `appimage-run` is sufficient initially. No Nix-specific provider binaries, package manager or runtime-linking project.
- webOS: test ordinary JS/QML/resource loading from application data on an unrooted target. Native-library execution is no longer required for providers. Use replacement IPK composition only if a real source/resource deployment limitation remains. Never implement it speculatively or compile C++ on the TV.
- Android/Android TV: Google Play distribution is required. Build the reviewed interpreted-source path with documented host access, signed packages and controlled imports; bundle official providers regardless. No downloaded native payloads or native self-updater in that channel. Recheck the actual current Play policy and submission requirements before release.
- Apple: same source packages/runtime, initially bundled. Obtain review/permission for the actual downloaded code and native UI/host interface before enabling independent downloads. Do not represent JS as automatic approval or enable an unapproved path later through a remote flag. Multiple bundled providers and configurable remote services remain available.

No platform should require provider authors to maintain separate JS implementations simply because its delivery policy differs. Platform-specific optional host capabilities must be declared and handled, not hidden in native binary variants.

## 12. Final parity, acceptance and cleanup

Before dropping native Jellyfin, verify the full inventory: authentication/QuickConnect, saved profiles and policy, discovery/manual server entry, all browse/filter/detail/person/list operations, playlists/collections/management, user state, playback and exact version negotiation, tracks/subtitles, transcode/device profiles/cleanup, reports, segments/trickplay, remote sessions and SyncPlay events/clock/queue/UI.

Provider-agnostic Spool watch-together is subsequent core work, not a blocker for the first JS comparison. Reuse generic native clock/drift/playback hooks; leave Jellyfin-native group interoperability optional. The proposed initial room relay is Worker/Durable Object + WebSockets, not STUN alone. Resolve each participant's own authorized stream and handle edition/timeline differences explicitly; never relay other participants' credential-bearing stream URLs as generic room state.

Run the multi-source matrix: two accounts in one provider plus another provider; overlapping IDs; source disable/removal during search/playback/UI actions; stale replies; failed/expired authentication; foreground browsing while another source plays; large simultaneous results; downloaded override/restart/recovery; missing optional/required extensions; low-memory pressure. Validate files/resources and source configurations across actual distribution packages.

Remove obsolete C++ protocol code, hardcoded provider globals, dead compatibility scaffolding and stale documents only once the replacement is genuinely used and tested. Keep useful fixtures, native core optimizations and deterministic contract tests. Do not keep both protocol implementations permanently merely to avoid a rewrite; the temporary native selector is a measurement/parity tool.

## 13. Working and reporting rules

Make small conventional commits. Do not push, publish tags, mutate remote repositories, install/launch on a TV or drive the user's desktop without the appropriate explicit authorization. Do not modify mpv for this provider work unless a demonstrated independent issue requires it; its submodule history is separate.

End the next session with an accurate handoff: commits, commands actually run, build/test results, effective backend now reachable, unsupported features, any data/schema changes, device/channel tests not run, benchmark provenance and the exact next implementation step. Report the first useful A/B milestone when it is reached; do not disappear into the full updater or a new framework first.

**Priority order:** validate these commits -> real JS catalog/account/artwork integration -> explicit native/JS comparison build with basic playback -> source-aware application and full Jellyfin/UI parity -> federated search/version selection and real Stremio -> verified independent delivery/channel testing -> cleanup/stabilization.

## References and evidence boundary

Repository paths and commit IDs above refer to the uploaded snapshot plus the local handoff commits. Original benchmark statistics come from the attached JSON files. Session-report statements are attributed reports, not tests rerun here. Estimates, budgets and sequencing are engineering decisions to verify.

- [R1] Qt, QQuickWindow: https://doc.qt.io/qt-6/qquickwindow.html — frame callbacks and rendering lifecycle.
- [R2] Qt Quick performance considerations: https://doc.qt.io/qt-6/qtquick-performance.html — GUI-thread budgets, batching and model/worker considerations.
- [R3] Qt Quick software adaptation: https://doc.qt.io/qt-6/qtquick-visualcanvas-adaptations-software.html — distinct non-accelerated rendering path.

The existing channel-policy document links official Google/Apple rules. Revalidate them during channel implementation/submission; this plan is not a store approval.
