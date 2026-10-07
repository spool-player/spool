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

## Limits

Loopback and protocol fixtures verify host behavior and request contracts; they do
not establish support on every server/device version. Live Jellyfin/Emby/Plex,
Plex Companion devices, two physical devices and TVs were not exercised. No TV
build/deployment, push, tag or release is part of this completion.
