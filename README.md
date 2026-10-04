# Spool

**Disposable experiment branch:** `experiment/trickplay-yuv-benchmark` launches
the onscreen RGB/YUV benchmark automatically. It is not intended for merging.
Use `--regular-app` to enter the retained normal application.

- libmpv: We've forked this and made it compatible in the directory above. keep libmpv behind a thin PlayerController / PlaybackController facade and do not let Jellyfin/network/UI code know about mpv internals.
- Qt6.11
  HTTP / REST: QNetworkAccessManager as the base, QRestAccessManager on top, plus QNetworkRequestFactory for shared base URL / headers / auth boilerplate. Use Qt’s JSON types (QJsonDocument, QJsonObject, QJsonArray) end to end. In Qt 6.11, QRestAccessManager is the REST-focused wrapper over QNetworkAccessManager, and Qt OpenAPI generates Qt HTTP clients using Qt Network APIs such as QRestAccessManager.
  API generation: Qt OpenAPI, not generic OpenAPI Generator, since you want the result to stay natively Qt-shaped. I would use it to generate the baseline Jellyfin client, then put a thin handwritten JellyfinApi facade over it for auth, session reporting, and whatever server-version weirdness you hit. Jellyfin does expose OpenAPI/Swagger docs, but there are recent reports of schema issues, so a wrapper layer is still wise.
  Database: QSqlDatabase with QSQLITE. That is the right choice for local cache/state in a client like this. Qt’s SQLite driver is in-process and single-file, and QSqlDatabase connections must only be used from the thread they were created in, so do DB work on a dedicated DB thread or keep strict per-thread connections.
  Async layer: upstream QCoro 0.12 on top of the Qt async APIs, so network/database orchestration stays coroutine-based. Native builds use the nixpkgs package, while webOS builds a pinned ARM package against the target Qt prefix.
  UI stack: Qt Quick + Qt Quick Controls with the Basic style as the base. Qt documents Basic as simple, lightweight, and giving maximum performance, which is exactly what you want for a TV UI that you will heavily customize anyway.
  Models: C++ QAbstractListModel exposed to QML, not ad hoc QML data blobs. QAbstractListModel is the standard one-dimensional model base, and both GridView and ListView are designed to consume C++ models like that efficiently.
  10-foot / remote navigation: build around GridView / ListView + FocusScope + KeyNavigation + Keys. KeyNavigation is specifically for arrow/tab-based focus jumps, and FocusScope exists to keep reusable focus regions sane, which is exactly the problem space for D-pad TV UIs.
  HTTP asset caching: QNetworkDiskCache for posters, backdrops, and image responses. It is basic, but it plugs directly into QNetworkAccessManager; just remember it is basic by design and defaults to a 50 MB limit, so you will probably want to raise that.

## Local sibling providers

Keep the app checkout and provider repositories together, for example
`~/Documents/spool/spool` and `~/Documents/spool/<provider-repo>`. From the app
checkout, these commands discover providers in immediate sibling directories
with valid `manifest.json` files; repository folder names do not matter:

```sh
nix run .#local-providers -- --dry-run  # Show discovery and paths; no build or launch
nix run .#local-providers              # Build the checkout and launch natively
nix run .#local-providers-build        # Build natively without launching
nix run .#local-providers-ipk          # Full local IPK build; no TV install or launch
```

The build-only and IPK commands also accept `-- --dry-run`. Discovery refuses
parents with more than 200 immediate directories and duplicate provider IDs.
These workflows enforce bundled-only provider loading and use discovered
checkouts instead of their locked versions; undiscovered locked providers retain
their pins. Native builds always use the working checkout, even on a clean Git
revision, rather than an immutable Cachix app package. They rebuild incrementally
on every invocation so sibling edits are included, using separate
`build/linux-release-local-providers` or `build/macos-local-providers` outputs.
Ordinary native builds continue to default to open provider loading without
local overrides. Set `SPOOL_REPO` to the app checkout when invoking outside it.

## Sign-in controls

Passwords and account PINs start hidden. Select the eye beside the input to show
or hide its contents; it also supports mouse/touch, Tab, and **OK/Enter**. On a
TV, **Right** from the input row focuses the eye and **Left** returns to the row.
Submitting or leaving the form hides the input again.

Device-link and Quick Connect codes have their instructions below the code box.
On desktop and mobile, **Copy** copies the code; TVs show it for entry on another
device without a clipboard control.

## Homepage library order

Drag a library card to another position on the homepage. Hold the pointer near
the left or right edge of the library row while dragging to scroll to libraries
that are offscreen, then release to drop it.

Alternatively, hold **OK/Enter** on a focused library, long-press its card, or
right-click it to enter move mode. The selected card shows **↔ Move**. Use
**Left/Right** on the remote or keyboard, or the visible arrow buttons, to move
that library. Press **OK/Enter**, **Back/Escape**, or **Done** to finish without
opening it. Moves are saved as they happen; Back does not undo them. Library
ordering does not reorder the recently added shelves.
Library badges use a descriptive server name when available. Default or
machine-generated server names are shown as the server's host and port instead;
usernames are not appended to library names.

## Local media folders

On desktop, choose **Add provider → This computer**, then add one or more folders
and confirm **Add library**. No Movies directory or other media source is added
implicitly. The selected folders and their subfolders form one library; overlapping
roots are deduplicated by canonical file path. Account settings let you change the
folder list later, including while the account is disabled.

Video thumbnails are extracted on demand with the bundled libmpv software renderer,
one file at a time off the UI thread. Qt encodes and caches the bounded thumbnail;
no external `ffmpeg` installation or visible playback window is needed. Changing a
file invalidates its thumbnail. Local favourite/resume state remains process-local.


## Episode details

Season details open the episode row at an in-progress episode, or the next
playable episode after the last watched one. A completed season starts at its
last watched episode. Episode details instead start at the episode being viewed.
That episode is aligned to the row's left edge, including at the end of a season;
earlier episodes remain available by scrolling left. The initial positioning
does not take focus from the main action or reset subsequent manual navigation.

## Desktop mpv configuration and keys

In expert playback settings, **mpv configuration** can be Off, Standard mpv
directory, or Custom directory. Off ignores user mpv files. Standard uses mpv's
own platform search paths; Custom uses the selected directory for `mpv.conf`,
`input.conf`, scripts and related files. Changes take effect on the next playback.
Spool never rewrites these files.

Precedence, from lowest to highest:

1. Spool's saved playback settings provide startup defaults.
2. Enabled user configuration overrides those defaults. mpv itself loads
   includes, profiles, scripts and bindings; Spool does not parse a subset.
   Compatible scaling, tone mapping, shaders, subtitles, audio filters and
   hardware-decoder choices are retained. Starting a file or detecting HDR does
   not reapply Spool's subtitle appearance over the user configuration.
3. Explicit playback controls and settings changes override their corresponding
   runtime options. Changing subtitle preferences applies only the changed
   options, rather than resetting unrelated user styling. A new playback loads
   the user configuration again; startup-only settings such as audio output and
   render quality remain defaults beneath that configuration.
4. Spool retains its embedding and session requirements: `vo=libmpv`,
   `gpu-api=auto`, `gpu-context=auto`, `wid=-1`, `force-window=no`, `idle=yes`,
   `keep-open=no`, `input-vo-keyboard=no`, `input-cursor=no`, `terminal=no` and
   `osc=no`. These are enforced before mpv initializes input, scripts or output.
   Spool also owns the initial window/fullscreen state, authenticated media
   requests, TLS verification, resume position, SyncPlay and explicit track
   restoration.

Desktop playback shares Qt's graphics device with libmpv: Vulkan on Linux,
Vulkan through MoltenVK on macOS, and D3D11 on Windows. The macOS app bundles
its Vulkan loader and MoltenVK driver; no Vulkan SDK or Homebrew installation
is required. MoltenVK requires a Metal-capable GPU. Select OpenGL in the
graphics backend setting, or launch with `SPOOL_RENDER_API=opengl`, for the
compatibility path. Adding MoltenVK does not establish macOS HDR output support.
Native `gpu-context` names such as `winvk` or `d3d11` are not mapped to embedded
backends. Scripts and dynamic profiles must not change the embedding options;
native-window commands, renderer replacement and standalone mpv playlist
management are unsupported. Config errors and embedding ownership are reported
in player/mpv diagnostics.

Desktop SDR/HDR selection follows the **current OS output mode**, not merely
the monitor's capabilities. SDR uses an RGBA8 video target with BT.709/BT.1886
output; mpv tone-maps HDR video into that SDR target, and the UI does not enter
the HDR conversion layer. FP16/scRGB is reserved for an HDR-enabled output.
Wayland uses the output's active transfer function, Windows its desktop DXGI
color space, and macOS its current EDR headroom. Unknown output state stays SDR.
Even the **Always** preference cannot force HDR into an SDR desktop.
The window's encoding is selected at startup: restart Spool after changing the
desktop HDR mode.


On webOS, supported codecs still use Starfish. For software-decoded codecs,
**Software video renderer** in advanced playback settings offers Automatic
(the established OpenGL `gpu` path), `gpu`, or experimental OpenGL `gpu-next`.
Both retain the lightweight software-video options. Changes apply to the next
playback; an explicitly selected renderer reports initialization failure rather
than silently switching backends. Vulkan is not offered on webOS.

During desktop playback, Spool's shortcuts, dialogs and focused text/IME
controls take precedence. Remaining keys reach mpv's `input.conf` machinery.
Printable characters retain their case, named keys and keypad keys use mpv
names, and modifiers are preserved (Command is `Meta` on macOS). mpv controls
repeat timing; Qt's synthetic repeat releases do not release a held key.
Leaving playback or losing window/focus ownership releases held keys.
mpv's built-in bindings remain off unless enabled with
`input-default-bindings=yes`.

For example, an `input.conf` can contain:

```text
F6 cycle-values speed 1 1.25
F7 cycle mute
F8 cycle fullscreen
F9 quit
```

`fullscreen` changes Spool's existing window, not a second mpv window.
`quit` (including `quit-watch-later`) ends the playback session and releases
the player; it does not close Spool. `stop` also returns to the application.
Bindings which manipulate volume, mute or speed update the corresponding Spool
controls. Closing Spool continues to use its normal application shutdown path.

## Disposable trickplay RGB/YUV benchmark

The bundled private fixture is the nearest-rank 95th percentile by compressed
JPEG size among 15,603 sheets inventoried from Jellyfin's trickplay directory:
1,548,147 bytes, 3200×1800, 10×10 thumbnails, 8-bit 4:2:0. The exact SHA-256,
source-relative path, population and selection rule are in
`tests/fixtures/trickplay-p95.json`. This is not a claim of 95th-percentile
decode latency. Do not publish the fixture with this temporary branch.

The three cases are the current production Qt RGB32 decoder, direct libjpeg
BGRA decoding, and direct libjpeg planar YUV420 decoding. All use the same
single-quad Qt Quick renderer, thumbnail geometry and fixed tile trace.
Direct RGB batches scanlines; YUV writes directly into MCU-padded planes and
uses actual one-byte GPU textures. GPU textures are reused for replacement
sheets. Crop-only frames never re-upload pixels. Both paths are opaque,
single-pass draws into the SDR window; no intermediate RGB framebuffer exists.

Each case runs three complete warm-ups, then three measured runs in a shuffled,
logged order. Every run contains 12 decode/upload replacements and 120
crop-only frames. The seed is logged. Default launch leaves the final result
onscreen; `--bench-auto-exit` makes desktop runs finite:

```sh
nix develop .#native -c cmake --build build/linux-release/app --target spool
nix develop .#native -c env SPOOL_RENDER_API=vulkan \
  build/linux-release/app/spool --bench-auto-exit --bench-seed 20261005 \
  --bench-log local-docs/trickplay-yuv-benchmark/optimized-vulkan.jsonl
nix develop .#native -c env SPOOL_RENDER_API=opengl \
  build/linux-release/app/spool --bench-auto-exit --bench-seed 20261005 \
  --bench-log local-docs/trickplay-yuv-benchmark/optimized-opengl.jsonl
```

`--bench-fresh-textures` restores the initial fresh-resource experiment.
`--bench-screenshot-dir PATH` chooses the diagnostic image directory.
Without an explicit log path, JSONL goes into the platform's persistent data
directory; the path is printed and shown onscreen. Every raw record also goes
to stdout with a `BENCH` prefix.

Logs retain warm-ups and measured records, wall/thread-CPU decode stages,
allocation sizes, queue/handoff times, synchronization, material updates,
per-plane upload enqueue, command recording and selection-to-Qt-swap latency.
Qt does not expose a truthful allocation/codec stage split, so those Qt fields
are null. GPU upload enqueue is not GPU execution time. Vulkan GPU events are
attributed to their originating request using Qt's frame-slot timestamp
lifecycle and include the whole Qt frame, uploads and drawing. Other backends
retain explicitly unattributed delayed GPU diagnostics; isolated GPU
upload/draw timings are always null.

Process RSS/anonymous memory has both a fixed pre-fixture baseline and
per-run baselines. An independent requested-1 ms sampler records approximate
run peaks; cumulative VmHWM remains separately labeled. Replacement decoding
can overlap the old and new CPU buffers. Decoded bytes, logical texture bytes
and Vulkan/D3D12 allocator counters are separate: none are a physical VRAM
measurement, and they must not be added blindly to RSS. Allocator caches and
driver/code pages can remain resident across runs.

After timing, the app reads back two thumbnails per case, including a
same-pointer crop change with `uploaded=false`, and verifies real rendered
pixels against CPU-decoded fixture crops. Screenshots and comparisons are
excluded from every performance metric. Treat results as valid only when the
final `complete` record reports both independent visual checks successful.
The early `fresh-vulkan.jsonl` run predates that check and is invalid because
its missing batchable shader variant produced blank previews.

## Performance benchmarks

GitHub Actions reports performance regressions as warnings, not build failures:
shared runners are too variable for reliable timing gates. Only overall
transition time (the sum of per-route median wall times) is compared. An increase
must reach both 25% and one frame budget; route timings, CPU shares, construction
costs and frame gaps remain diagnostics, not constraints on new approaches.

**Follow-up: run these benchmarks on dedicated hardware before restoring
performance gates.** Record the baseline on the same hardware and rendering
backend. `tools/compare-render-benchmark.py` supports strict local comparisons;
CI passes `--warn-only`. Empty or broken measurements still fail rather than
being mistaken for good performance.

Provider form layouts are precompiled in the host. Settings creates only visible
rows and opens its native folder chooser on demand; initial viewport readiness is
checked at the end of the current event-loop turn, with timed retries only when
delegates are still missing. Playback previews prime the resume-position
texture on a dedicated network/decode path, independent of poster queues.
Sprite sheets up to 50 MB decoded RGB32 (12.5 megapixels) stay resident:
moving between tiles changes the displayed crop without another image load,
CPU crop, or texture upload. Larger JPEG sheets use codec-clipped frames.
Whole BIF sequences use their native timestamp indexes, not an assumed
uniform interval. Qt's existing JPEG plugin supplies its libjpeg/libjpeg-turbo
decoder; no duplicate JPEG dependency is needed.

The server-free native fixture smoke exercises real JPEG sprite and BIF pixels,
malformed BIF boundaries, resident-sheet crops, the 50 MB boundary, directional
prefetch, and stale-session/late-request cancellation:

```sh
nix develop .#native -c cmake --build build/linux-dev/app --target app-tests providers-tests
nix develop .#native -c env SPOOL_TRICKPLAY_SMOKE_OUTPUT=/tmp/spool-preview-frame.png \
  build/linux-dev/app/app-tests trickplay
nix develop .#native -c ctest --test-dir build/linux-dev/app \
  -R '^(trickplay|artwork-authentication|playback-timeline)$' --output-on-failure
```

The smoke prints cold response time, 24 explicit warm image-provider response
times, full-sheet decode time, and texture byte counts. Normal hover within a
resident sheet does not issue those additional image-provider requests.
These measurements exclude actual GPU transfer and presentation duration.
Each local/remote session caps retained decoded textures at 50,000,000 bytes
and encoded sheets (or one BIF sequence) at 32 MiB. The decoded budget is not
a total RAM/VRAM limit: decoder workspace, active image delivery and GPU
textures add to it. Sequences over 32 MiB are rejected.

Hover image selection consumes the latest input once per rendered frame;
the timestamp and preview position update immediately. Prefetch follows hover
direction to one neighbouring sheet or at most two BIF frames. Foreground
requests preempt speculative work; a neighbouring sheet is decoded only when
it fits beside the current retained textures, otherwise only its encoded bytes
are prefetched. Qt image caching is disabled for previews so historical sheets
do not accumulate in its image cache. Switching sessions cancels outstanding
delivery and drops caches.
Remote resource revisions also change when a target switches preview URL or
credentials for the same item. Retired revisions cannot reuse cached frames;
unchanged resources keep stable identities across state polling.



## Android development

The Android toolchain is pinned to SDK 36, Build Tools 36.0.0 and NDK
27.2.12479018 by the flake, and to the Qt and FFmpeg versions in
`tools/manifests/toolchain.json`, which every platform reads. `nixpkgs` tracks `nixos-unstable`; the
headless emulator and its Google APIs x86_64 system image come from that
channel rather than nixpkgs master.

Build the emulator ABI locally, in one command:

```sh
nix develop .#android -c bash tools/android/build.sh
```

That runs the three cached stages -- `build-dependencies.sh`, `build-qt6.sh`,
`build-apks.sh` -- which can also be invoked on their own.

Entering the Android shell may build native Qt host tools before the Android
cross-build starts. These must match the pinned Qt version: `moc`, QML generators,
`qsb` and translation tools run on the build machine. The host profile omits
Quick/Controls and desktop-only dependencies; Qt GUI libraries remain necessary
for the generators. The emulator is provided separately by `nix run .#android-emulator`.

The build produces `spool-x86_64.apk` under `dist/android`. One package serves both
phones and televisions -- the form factor is asked of the system at runtime --
so architecture is selected with `ANDROID_ABI`, not separate phone/TV builds.
The CMake `TOUCHSCREEN` option defaults on for Android and off elsewhere.
Enabled builds activate mobile player interactions only on non-TV devices:
Back exits playback immediately, and tapping the video outside the controls
toggles the OSD. Android TV retains remote-oriented navigation. Future mobile
targets can enable the same option without Android-specific QML.

Music continues in the background through an Android media-playback foreground
service, with system/lock-screen controls for play, pause, seek, queue navigation
and stop. Audio focus loss and headphone disconnection pause playback. Local
music takes priority over remote-control notifications while it is active.
Video still pauses when the app is hidden; picture-in-picture overlay playback
is not implemented.

Automatic picture-quality adjustment restarts an active stream at its saved
position, retaining pause, selected tracks and per-file sync delays. On Android,
the ladder can ultimately switch from Enhanced to Direct MediaCodec output.
Handset 100% uses a calibrated dp baseline; card counts follow available width
and zoom rather than a fixed phone grid. The Android Qt build carries a
live-density notification patch so fold/configuration changes update the UI
without requiring an app restart.

New installations start at 100% interface scale on every platform, including
webOS and Android TV. Existing saved percentages are preserved. Scale always
stays device-local and is excluded from settings synchronization.

`tools/android/build-universal-apk.sh` merges per-ABI APKs from one build into
the single `spool-universal.apk` the release page offers, for people who do not
know their device's architecture. The in-app updater never fetches it: the
update manifest is keyed by ABI and that file is deliberately absent from it.

Signing uses the real upload key when
`~/.local/share/spool/signing/android-upload-credentials.json` exists, which
is what makes a local build installable over an app already on a device:

```json
{
  "keystore": "/home/you/.local/share/spool/signing/android-upload.p12",
  "alias": "spool-upload",
  "storePassword": "...",
  "keyPassword": "..."
}
```

Keep it and the keystore outside the repository. Without it the build falls
back to a debug key generated at `build/android/debug.keystore`, which
installs on a clean device and nowhere else. Launch-test both variants in the pinned headless emulator with:

```sh
nix develop .#android -c bash tools/android/emulator-launch-test.sh
```

On Android, **Export diagnostics** packages the app log, mpv log, rotated
logs, and system report into a ZIP and opens the system share menu. It does
not require ADB or broad storage permissions.

# Name

## Fast media player for LG TVs and desktop

// Download link box

// quick headline Features dot points

// links to below sections

// screenshots

// full feature list

// usage information

// technical information

// thank yous (kodi)
