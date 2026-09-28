# Provider parity verification

The provider parity plan is implemented across Spool and the sibling Jellyfin,
Emby and Plex providers. The API remains 0.2; optional features negotiate exact
extension versions per account. Local provider builds use the sibling working
trees. Published provider pins are unchanged until a separately authorized release.

Verified on 2026-09-29: the local-provider release build succeeded, all 94
non-GPU tests passed, all six provider contract runs passed, and all three
provider archives passed package validation.

## Reproduce the checks

From the Spool repository:

```sh
nix run .#local-providers-build
nix develop .#native -c ctest --test-dir build/linux-release-local-providers/app \
  -E '^mpv-video-item' --parallel 8 --output-on-failure
```

The two excluded mpv rendering tests require a GPU. Provider contracts also run
against each sibling's `tests/contract.mjs` with `provider-contract-runner`, both
normally and with `QV4_FORCE_INTERPRETER=1`.

`extension-integration` uses real Settings, SettingsSync, ProviderRegistry,
SourceHub and RemoteTargets controllers, temporary storage, a stateful loopback
HTTP server and actual QML surfaces. It verifies native and Spool document writes,
confirmed read-back states, keyboard sync opt-out, stale responses after an account
change, remote target selection without playback, removal of the second duplicate
queue entry, and rejected/successful PIN submission through a provider surface.
Existing native and provider tests cover service-specific protocols, activation
families, generation races, old-host compatibility and settings convergence.

For screenshots, supply an output directory:

```sh
nix develop .#native -c env SPOOL_INTEGRATION_CAPTURES=/tmp/spool-extension-captures \
  ctest --test-dir build/linux-release-local-providers/app \
  -R '^extension-integration$' --output-on-failure
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
