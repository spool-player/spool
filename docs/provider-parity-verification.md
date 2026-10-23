# Provider parity verification

The provider parity plan is implemented across Spool and the sibling Jellyfin,
Emby and Plex providers. The current contract uses manifest format 3 with one
declared-and-account-offered boolean capability map, not API or extension-major
negotiation. Local provider builds use the sibling working trees.

## Current capability cutover (2026-10-08)

The Linux Qt 6.11.2 native build succeeded for the production host, `spoolet`,
native provider/app contracts, SDK contract runner and shared QML test target.
Focused verification passed 17 native selectors covering capability withdrawal,
activation, origins, cancellation, preference writability, bounded storage/CAS,
remote targets, collections, downloads and the bundled Jellyfin consumer.
Five isolated offscreen GUI contracts passed: artwork authentication, provider
screen context, QML cache, settings/remote/activation integration and provider forms.
These are targeted checks, not a full platform matrix or native-GPU claim.

All five format-3 working archives passed SDK validation. Their full provider
contract suites ran against the **unpacked archive logic**, not just checkout
source, in both default and forced-interpreter Qt modes (ten successful runs).
The real Stremio loopback HTTP smoke also passed manifest/catalogue/search,
stream picker, HEAD validation, original media bytes and torrent-file selection.
The SDK package suite (11 cases), checkout discovery suite (eight cases),
curated working-package policy contract and all five SDK hash checks passed.

The first Qt consumer runs exposed unsupported `Object.fromEntries` in the
new Jellyfin/Emby/Plex capability construction. Explicit loops fixed the real
runtime defect; the six affected archived-package runs and both bundled
Jellyfin selectors then passed. No polyfill or compatibility shim was added.
Bundled versions are Jellyfin 0.2.11, Emby 0.1.7, Plex 0.1.8, Stremio 0.1.2
and Open Movies 1.1.1. They are working packages, not published releases.
Curated release URLs and digests await separately approved publication.

Independent review then identified active group ownership surviving withdrawal
when another account kept aggregate availability true. The existing
`group-playback-policy` test now drives two real registry accounts through the
public group controller and capability events. It failed before repair at the
active/pending group ownership assertion, then passed after the controller consumed
account-specific support changes and used its existing group teardown. Withdrawal
of the unrelated account leaves the joined group intact. The focused followup
build and four selectors (`group-playback-policy`, `group-clock`,
`provider-registry`, `source-hub`) passed.

A subsequent real-display run on a headless Weston compositor exposed
a harness prerequisite rather than withheld capabilities: at the remote chooser
boundary Qt reported `ApplicationInactive`, the native window was inactive,
remote availability was true, no request was busy, and only the local target
existed. Qt Quick Test also reported that `requestActivate()` had not activated
the window. Production correctly suppresses discovery while backgrounded.
The retained integration test now requires actual native window/application
activation before exercising foreground-only behavior; its original target,
queue and PIN assertions are unchanged. The updated offscreen contract passed.
Real GPU/display acceptance still requires the isolated launcher to provide
working native input/activation; offscreen success does not establish that.

## Earlier parity verification

Verified again on 2026-09-30 after the provider UI/local-library cutover: the
local-provider release build succeeded, all 98 non-GPU tests passed, all six
provider contract runs passed, and all three provider archives passed validation.

Additional isolated checks exercised the real Jellyfin password form, Emby Connect
PIN/membership selection and protected Plex Home flow against scripted provider
contexts. A loopback server required the generated Jellyfin trickplay URL's account
token, and Qt fetched and rendered the expected colored pixels. Local-provider tests
exercise opt-in setup/cancellation, overlapping folders, configuration while disabled,
re-enabling the changed library, and decoded/cached video thumbnail pixels.
The artwork bridge regression additionally checks inherited image ownership and
rendered pixels for native/JavaScript-backed rows through first population,
replacement and resizing. It also passed with the application's compiled QML
cache objects linked into the isolated runner. Enabled and disabled login button
fills were checked as blue in an offscreen render.

The same three-pass, cold-route offscreen software benchmark measured Settings median
wall time at 44.0 ms before and 31.1 ms after, and GUI CPU time at 22.8 ms before and
13.6 ms after. Settings constructed 9 rather than 13 delegates. Other routes' construction
costs were lower or similar, but presentation waits varied and did not improve uniformly.
These are isolated empty-library measurements, not a claim about GPU presentation,
large live-server libraries, or physical input-to-display latency.

## Reproduce the checks

From the Spool repository:

```sh
nix run .#local-providers-build
nix develop .#native -c python tools/run-tests.py \
  --build-dir build/linux-release-local-providers/app --workers 8
```

The unified driver now includes the real GPU consumers on an isolated Linux
display; it no longer excludes mpv selectors. Provider contracts also run
against each sibling's `tests/contract.mjs` with `provider-contract-runner`, both
normally and with `QV4_FORCE_INTERPRETER=1`.

`extension-integration` uses real Settings, SettingsSync, ProviderRegistry,
SourceHub and RemoteTargets controllers, temporary storage, a stateful loopback
HTTP server and actual QML surfaces. It verifies native and Spool document writes,
confirmed read-back states, keyboard sync opt-out, stale responses after an account
change, remote target selection without playback, removal of the second duplicate
queue entry, and rejected/successful PIN submission through a provider surface.
Existing native and provider tests cover service-specific protocols, activation
families, generation races, capability withdrawal and settings convergence.

For screenshots, supply an output directory:

```sh
nix develop .#native -c env SPOOL_INTEGRATION_CAPTURES=/tmp/spool-extension-captures \
  bash tools/test-gpu-session.sh \
  build/linux-release-local-providers/app/spool-e2e-tests --child extension-integration
```

The test captures both Settings and Subtitle Appearance at 1280×720, 1920×1080,
and 3840×2160, at 100% and 150% zoom, in confirmed, pending, error and opted-out
states: 48 images. It uses the application's bundled fonts and offscreen software
rendering. It does not open a desktop window or contact a real media server.

## Real application journey coverage

`app-journey` launches the built Spool product and drives its existing `spoolet`
interface, rendered labels and normal pointer/remote input. The shipped Jellyfin
provider talks to a private authenticated loopback server; the fixture supplies
protocol responses and records requests, not navigation or playback decisions.
The journey extends login, settings, download/offline playback and stale-progress
coverage with these transitions:

| Transition | Observable proof | Regression caught |
| --- | --- | --- |
| Played filter → empty library → Reset → remote OK | Server sees `IsPlayed`; browse becomes empty, then restores both items; Back closes only the menu and OK opens the movie | Filter lost in provider translation, stale results, or focus stranded after model replacement |
| Movie → related episode → Back → movie → Back → library → remote OK | Real episode details are fetched; distinct episode/movie headings establish restored identity and remote selection | Retained details rebound to the child, skipped parent route, or lost browse selection |
| Repeat current item → actual media EOF | New playback negotiation and server start receipt at zero with `RepeatOne`, followed by decoded red-over-blue beginning frames | Menu changes only visual state, repeat fails to restart, or restart incorrectly resumes old progress |
| Timed sleep → After current item with repeat still enabled → EOF | Remaining-time and after-current labels; end-position stop receipt, watched mutation, and no further negotiation/playback during a bounded observation window | Timer replacement fails or repeat takes precedence over the requested stop |

Run the already-built journey in an isolated Linux GPU session:

```sh
nix develop .#native -c bash tools/test-gpu-session.sh \
  build/linux-release-local-providers/app/spool-e2e-tests --child app-journey
```

Build `spool-e2e-tests` (which depends on the matching `spool` and `spoolet`)
first. The native shell supplies the pinned display/Mesa runtime; FFmpeg and
Tesseract with English data must also be available. Each wait has a deadline;
EOF cases seek the paused, finite 90-second fixture near its end rather than
waiting for full playback or altering the product clock. Existing Android/tvOS
adapters run the same journey against driver-owned isolated emulators/simulators;
the test does not accept a user device. Do not substitute Qt offscreen/minimal or
the Qt Quick software renderer for this GPU journey.

Failures retain private screenshots, allowlisted fixture counters and app logs in
the reported artifact directory. Do not publish raw logs or control descriptors.
These cases are executable coverage, not a claim of a completed platform run;
record actual execution results separately.

## Limits

Loopback and protocol fixtures verify host behavior and request contracts; they do
not establish support on every server/device version. Live Jellyfin/Emby/Plex,
Plex Companion devices, two physical devices and TVs were not exercised. No TV
build/deployment, push, tag or release is part of this completion.
