# AGENTS.md

## Project

- Spool for Jellyfin client for webOS and desktop.
- Prefer a smaller, faster, easier-to-extend codebase over compatibility scaffolding or speculative abstractions.
- The app is prerelease software: bump/reset caches on schema changes instead of adding migrations or fallback readers.
- `mpv/` is a submodule. Commit mpv changes inside `mpv/`, then commit its pointer here.
- Make small conventional commits as coherent work finishes. Never push unless the user explicitly asks.
- After any push or tag, watch the Actions runs it triggers and report the first
  failure the moment it appears. A release is not delivered because it was
  tagged; it is delivered when its runs are green.
- Watch **jobs, not runs**. `gh run watch` and `gh run view --exit-status` only
  return once every job has finished, so a matrix whose Windows leg failed in
  two minutes stays silent until the slowest leg finishes an hour later. Poll
  the job list and break on the first failed job:

  ```sh
  until gh run view "$id" --json jobs \
      --jq '.jobs[] | select(.conclusion == "failure") | .name' | grep -q .; do
    gh run view "$id" --json status --jq .status | grep -q completed && break
    sleep 60
  done
  gh run view "$id" --log-failed
  ```

  Run that in the background per run id, and read `--log-failed` for the failing
  job alone rather than downloading the whole log.

## Module Seam

The goal is one core that several backends can sit on (spool-jellyfin,
spool-plex, spool-stremio). `CMakeLists.txt` groups sources into
`SPOOL_PLATFORM_SOURCES`, `SPOOL_PLAYER_SOURCES`, `SPOOL_SHELL_SOURCES` and
`SPOOL_JELLYFIN_SOURCES` to mark where that cut goes. They still build as one
library; the grouping exists so the split stays mechanical.

- Core (the platform, player and shell groups plus everything under
  `src/platform`, `src/player`, `src/media`, `src/provider`, `src/common`,
  `src/cache` and `src/diagnostics`) never includes `src/api/` or
  `src/discovery/`. `tools/check-module-seam.sh` enforces that and runs as
  the `module-seam` ctest; `--strict` also treats every file still listed in
  `SPOOL_JELLYFIN_SOURCES` as provider and currently reports one leftover
  (`DatabaseManager` reaches `AccountProfile`).
- A media source is a `Provider` (`src/provider/Provider.h`): an id, a
  display name, a set of capability flags and a `PlaybackSource`.
  `ProviderRegistry` holds the active one and publishes its flags as the
  `ProviderCapabilities` QML singleton (eleven booleans: auth, discovery,
  search, userItemState, playbackReporting, segments, libraryManagement,
  syncPlay, remoteControl, quickConnect, peerRelay). Shared QML gates every
  provider-specific control on one of those names; the provider registers
  the singletons only its own QML reaches (`Session`, `SyncPlay`,
  `RemoteControl`, `Management`, `QuickConnect`, `Discovery`,
  `DiscoveredServers`) from `registerQmlSingletons()`.
- `JellyfinProvider` (`src/api`) composes everything Jellyfin-specific: the
  facade, discovery and its cached server list, the session, QuickConnect,
  remote control, and after `attach()` SyncPlay, library management and the
  settings bridge. It owns the session-driven lifecycle of those parts;
  `AppController` is the composition root above it and constructs none of
  them. Nothing under `src/platform` or `src/diagnostics` includes
  `AppController`: the platform layer takes `ApplicationHooks` and the render
  benchmark `RenderBenchmarkHooks`, both filled in by `main.cpp`.
- The player talks to its media source only through
  `src/provider/PlaybackSource.h`; `JellyfinApiFacade` implements it.
  `SettingsController` emits the preferences a source needs and takes the
  account's remote settings back through `applyRemote*()`;
  `JellyfinSettingsBridge` in `src/api` is the Jellyfin side of that. Its
  schema drops the account rows when the active provider has no `auth`.
  `configurePlatformPlaybackCapabilities()` takes an applier callback, not
  the facade.
- `qml/primitives` and `qml/theme` reach exactly two singletons, `Art.url` and
  `Settings.uiScalePercent`. Keep it that way; page- and shell-level QML is
  where backend-shaped data belongs.
- Keep backend branding out of shared code. Settings copy, theme colour names
  and network user agents should read as the product, not as Jellyfin.

## Pinned Versions and Generated Assets

- `tools/manifests/toolchain.json` sets the Qt and FFmpeg versions for every
  platform. Nothing else may name one; `tools/toolchain-versions.sh --check`
  is what CI runs to keep the Qt module manifests honest.
- The launch screen carries the version, so it is rendered per build by
  `tools/generate-splash.sh` from `tools/manifests/splash.json` and is not
  committed. Configuring the app is enough to get it on every platform.

## Local Development

- Launch the native release app with `nix run`. Clean Git revisions substitute the immutable package from Cachix; dirty tracked changes use the incremental checkout build.
- Launch the already-built native release app without rebuilding with `nix run .#run`. It fails with a build-command hint when the binary is missing.
- Build without launching with `nix run .#build`.
- Run the test suite the way CI does with `nix run .#tests` (release build, then the same ctest invocation and exclusions as the workflow).
- Leave interactive UI testing to the user. Do not drive their desktop with xdotool/xdgtool or similar input automation, or launch visible smoke tests unless explicitly requested. Use builds and isolated/offscreen checks for verification.
- Use `nix develop .#native -c ...` for targeted development commands (e.g. `cmake --preset linux-dev`, then `cmake --build build/linux-dev/app --target jellyfin-native`).
- The image-diagnostics equivalents remain `nix run .#image-debug-build` followed by `nix run .#image-debug`.
- Batch coherent edits, then run one build and one `qmlformat`/`clang-format` invocation over all touched files; don't build or format file-by-file.

## webOS

- Do not build, install, launch, deploy, or test on a TV unless the user explicitly requests it. "Deploy" means install-only; never launch unless separately asked.
- For a requested deployment, use this exact build-and-install command from the repository root:
  `TV_HOST=root@tv.local; ./build-ipk.sh && nix develop -c bash tools/webos/verify-device.sh --no-launch --host "$TV_HOST" ./build/com.sachk.spool_0.3.0_arm.ipk`
  `build-ipk.sh` defaults to the complete fresh `all` pipeline. Never invoke the `app`, `stage`, or `package` phases separately.
- The packaged IPK is the only supported way to update the TV app. Never copy binaries or libraries to the TV manually, mutate the installed application tree, or substitute hand-written CMake/staging/install steps.
- If webOS diagnosis is requested, read current logs first and make one targeted change — no speculative deploy loops.
- Never write to TV partitions, patch package metadata, restart system services, or wipe app/user data.
