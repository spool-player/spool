# Spool

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

Use the Nix launcher rather than invoking the installed binary from an unrelated
development shell: it supplies the matching Qt plugins and, on Linux,
`libsecret`'s `secret-tool`. Account configuration stays in the platform
credential store; the SQLite database contains account metadata, not a copy of
the credentials. If a scripted provider's saved configuration is missing,
unreadable, malformed JSON, or not a JSON object, that account reports a failed
connection and offers sign-in instead of starting with unavailable settings.
A stored empty JSON object (`{}`) is available configuration: public or anonymous
providers can restore it normally. Providers remain responsible for validating
their own authentication configuration; an empty object does not bypass
first-party activation validation.
Unlock the credential store and restart Spool, or sign in again. Changing or
removing another account does not overwrite unavailable credential records.
A missing SQLite driver fails startup without resetting account data or clearing
the credential store.

Ordinary startup opens Home, combining the selected saved viewer from every
independent server. **Profiles & servers** in the navigation bar groups saved
profiles by server and lets you switch who's watching, or add another watching
profile through that provider's sign-in screen. A switch stays on the profile
page while it connects or asks for a PIN; cancellation or failure keeps the
previous viewer and offers retry/reconnect instead of pretending the switch
succeeded. Protected profiles that need interaction stay locked on startup.
Search uses only the viewers selected for Home, never the combined permissions
of an adult and child profile saved on the same server.

### Offline downloads

Open an item's menu and choose **Download…**. Original media is offered where
the item's provider supports it; **Server-converted** quality choices appear
only when that provider offers a complete server-encoded file. Spool never
encodes media on the device or saves an online playlist as an offline movie.
**Settings → Downloads** shows preparation, byte progress, completion and
actionable errors, with Cancel, Retry, Play offline and Remove actions.

Completed media and its local metadata persist across restarts and account
removal. The built-in **Downloads** LocalProvider library plays the local copy
without server headers or an active connection; offline resume/played state is
stored locally. Interrupted operations become retryable failures after restart
and discard incomplete files. A retry negotiates fresh access with the provider.

LG webOS builds disable offline downloads by default to protect the TV's limited
internal storage. With a suitable USB drive connected, enable **Settings →
Downloads → Allow downloads**, then choose a writable folder on that drive.
This opt-in stays local to the TV and is never synced. Turning it off cancels
active downloads and blocks new downloads/retries, without removing saved media
or disabling offline playback. Spool does not detect USB drives or reserve free
space automatically; check the destination and available space before enabling.

Desktop defaults to `Movies/Spool` and has a native folder chooser. On Android,
the destination control is under Advanced: choose internal/external app storage
or a Storage Access Framework folder with a persisted read/write grant.
Document providers must permit creation/rename and provide seekable media;
revoked grants or unavailable storage surface errors rather than buffering a
copy into memory. iOS/tvOS use app-managed sandbox storage without an unsupported
folder chooser; webOS defaults to app-managed writable storage and accepts an
accessible folder path. Changing destination affects new downloads only.
Downloaded media can be large: check free space and remove copies when finished.

### Built-in local automation (`spoolet`)

Native desktop builds also build/install `spoolet`, a small Qt Core/Network CLI
from this checkout, with no browser or external control service. The ordinary
Spool desktop app starts its private local command server automatically:

Desktop `--data-dir PATH` starts with a separate database, INI settings, caches,
logs, diagnostics, provider installation directory and default download folder.
It does not import or move the normal profile's data. OS-managed credentials
remain in the platform credential store; selecting a data directory does not
silently replace that store with plaintext files. Isolated fixture runs opt into
the existing private file backend with `SPOOL_CREDENTIAL_STORE_DIR` and must
never use real account secrets. Mobile builds retain their platform sandbox and
do not accept `--data-dir`.

```sh
nix run .#local-providers-build
nix run .#local-providers -- --instance plex-check
# In another terminal; uses the already-built checkout, without rebuilding:
nix run .#spoolet -- help
nix run .#spoolet -- instances
nix run .#spoolet -- --instance plex-check status
nix run .#spoolet -- --instance plex-check navigate home
nix run .#spoolet -- --instance plex-check key right
nix run .#spoolet -- --instance plex-check key ok
nix run .#spoolet -- --instance plex-check back
nix run .#spoolet -- --instance plex-check items libraries
nix run .#spoolet -- --instance plex-check library ACCOUNT_PREFIX:LIBRARY_ID
nix run .#spoolet -- --instance plex-check items browse 0 100
nix run .#spoolet -- --instance plex-check play ACCOUNT_PREFIX:ITEM_ID
nix run .#spoolet -- --instance plex-check qualities
nix run .#spoolet -- --instance plex-check quality 0
nix run .#spoolet -- --instance plex-check pause
nix run .#spoolet -- --instance plex-check seek 120
nix run .#spoolet -- --instance plex-check resume
nix run .#spoolet -- --instance plex-check screenshot /tmp/spool-playback.png
nix run .#spoolet -- --instance plex-check stop
```

Use the qualified IDs returned by `items`, not guessed raw provider IDs.
`items resume`, `items next-up`, and `load-more` expose the existing loaded
models; browsing and playback requests are asynchronous, so poll `status`
(`initialized`, `busy`, `homeLoading`, `browseLoading`, `playback.loaded`) before
acting on their results. `qualities` reads the actual application's current
options, including selected Auto/Original/fixed ceilings, rather than a separate
CLI ladder. `quality INDEX` selects one of those options using the real playback
API; subsequent stream negotiation can take time.
The credential-free `source` snapshot reports the actually resolved edition's
bitrate, pixel dimensions, aspect-normalized quality height, resolution label,
and playback method. For example, a 3840×1608 edition is a 2160-class source
despite its cropped pixel height; the CLI never reconstructs quality choices
from the negotiated output or invents a source ladder.

Preview inspection uses real window input, without seeking the video. For this
latency-sensitive sequence, invoke the installed `spoolet` binary directly:
the Nix wrapper can take about three seconds to start, enough for playback
controls to hide between commands. The Nix launcher above remains available
for ordinary CLI use. From the checkout root on Linux:

```sh
SPOOLET=./build/linux-release-local-providers/install/bin/spoolet
"$SPOOLET" --instance plex-check key up
"$SPOOLET" --instance plex-check preview 120
"$SPOOLET" --instance plex-check state
# Poll state until visual.preview.active is true, selectionPending is false,
# and visual.previewTexture.ready is true, then capture immediately:
"$SPOOLET" --instance plex-check screenshot /tmp/spool-preview.png
"$SPOOLET" --instance plex-check settings get
"$SPOOLET" --instance plex-check settings set playback/seekPreviews false
"$SPOOLET" --instance plex-check settings set playback/seekPreviews true
"$SPOOLET" --instance plex-check settings set playback/accurateTrickplay true
"$SPOOLET" --instance plex-check settings set playback/trickplayPreviewScalePercent 150
"$SPOOLET" --instance plex-check settings set appearance/uiScalePercent 125
"$SPOOLET" --instance plex-check settings set playback/renderQuality '"high"'
```

`preview` needs available trickplay and visible playback controls; it hovers the
real seek bar. `pointer move X Y` / `pointer click X Y` use window logical
coordinates for other precise UI interactions. Settings go through the app's
existing schema validation and persisted user-change transaction. Local UI
scale changes do not make this device-specific setting remotely syncable.
Seek preview size is relative to interface scale and does not change the
server's thumbnail resolution; the two scale settings are independent.
The CLI exposes only the five settings shown above, not credentials or arbitrary
configuration. `state` is a small allowlisted route/playback/visual snapshot,
not unrestricted QObject inspection or script evaluation.

Download automation uses the same provider negotiation, quality options and
native transfer manager as the UI:

```sh
"$SPOOLET" downloads options ACCOUNT_PREFIX:ITEM_ID
"$SPOOLET" downloads start ACCOUNT_PREFIX:ITEM_ID 0
"$SPOOLET" downloads list
"$SPOOLET" downloads cancel JOB_ID
"$SPOOLET" downloads retry JOB_ID
"$SPOOLET" downloads play JOB_ID
"$SPOOLET" downloads remove JOB_ID
```

Choose the index returned by `downloads options`; zero is Original.
Starting is asynchronous and can open the provider's edition/stream picker.
Poll `downloads list` for job state and transferred/total bytes. The output
excludes destination paths, media URLs, credentials and server cleanup data.
`downloads play` launches a completed local copy without remote playback relay.

For pointer testing, `pointer press X Y`, frame-paced `pointer move X Y`,
and `pointer release X Y` perform a real held-button drag; `pointer right-click
X Y` opens context menus. Coordinates are window-logical pixels. A normal library
drag scrolls; choose Move from its menu before testing drag-and-drop reordering.

Every command writes one machine-readable JSON result to stdout; failures exit
nonzero and never prompt. `--timeout MS` (100–30000, default 10000) bounds the CLI
request. The server limits each connection to one newline-framed JSON request,
64 KiB input, 1 MiB output, eight concurrent connections, and a ten-second
deadline. Specify `--instance ID` when more than one app is running; unnamed
launches get a unique discoverable ID. An explicitly named duplicate launch
fails rather than silently controlling another instance. `SPOOL_INSTANCE`
sets the launch identifier; `--no-local-control` disables the server.

This is **same-user local IPC only**: Unix-domain sockets or Windows named pipes
with Qt's user-access restriction, plus a private discovery capability. On Unix,
the runtime directory is owner-only and discovery files are owner-only regular
files; unsafe/symlink discovery is rejected. Discovery lives in the user's
runtime directory (`spool-control`, user data directory on Windows), is removed
on clean shutdown, and stale entries are ignored after probing. No TCP/WebSocket
listener is opened. Endpoint capabilities and provider credentials are not
returned by the CLI. Anyone already running code as your user can control the
app; treat screenshots and media titles as private.

Screenshots wait for a newly swapped Qt frame, read the actual window framebuffer,
and atomically save an owner-only PNG with path, pixel dimensions, file size and
state metadata. Scene-graph embedded video is included; video on a separate
native plane/external window is excluded and explicitly reported with
`videoIncluded: false`. PNG is not a calibrated HDR capture. The window must be
exposed; screenshot completion does not mean a provider image has finished
loading—check the reported playback/preview readiness first. The server and CLI
are desktop facilities, not a public remote-control API or a TV deployment tool.

Without Nix, use `spoolet` beside `spool` in the native build directory or installed
`bin/`. Local-provider Nix builds install it to
`build/linux-release-local-providers/install/bin/spoolet` (macOS:
`build/macos-local-providers/run-install/bin/spoolet`); the `.#spoolet` launcher
uses these exact checkout outputs.

### Local playback URL diagnostics

URL logging is redacted by default. To inspect the complete failed playback URL
from a local checkout, explicitly opt in for that process:

```sh
nix run .#local-providers -- --unredacted-urls
```

The launcher forwards this flag to the real app, which warns at startup.
Playback failure and mpv/curl messages in `spool.log` retain the full URL,
including the server address, encoded client-profile parameters and any URL
credentials. On Linux this log is normally
`~/.local/share/spool/Spool/logs/spool.log`; startup prints the actual location.
Other password/token fields and authorization headers stay redacted. The
unsanitized standalone mpv file sink is disabled: mpv messages use the same Qt
sanitizer and application log. Existing `spool-mpv.log` files from older runs are
not sanitized retroactively.

**Keep opted-in logs private. URLs may contain working credentials.** Do not
upload or share them without removing secrets, addresses and identifiers.
Restart without the flag to restore default URL redaction. For providers that
authenticate media through HTTP headers (including Plex), reproducing the
request also requires the account's private media headers; a URL alone is not
an authenticated request.

## Sign-in controls

Passwords and account PINs start hidden. Select the eye beside the input to show
or hide its contents; it also supports mouse/touch, Tab, and **OK/Enter**. On a
TV, **Right** from the input row focuses the eye and **Left** returns to the row.
Submitting or leaving the form hides the input again.

Device-link and Quick Connect codes have their instructions below the code box.
On desktop and mobile, **Copy** copies the code; TVs show it for entry on another
device without a clipboard control.

## Homepage libraries

Drag normally to scroll the library row horizontally. Right-click a library,
long-press its card, or hold **OK/Enter** (or press **Menu**) on a focused library
to open its menu: **Move**, **Hide library**, and **Show hidden libraries**.

Choose **Move** to enable drag-and-drop ordering. Hold the pointer near the left
or right edge while dragging to scroll to offscreen libraries, then release to
drop. In move mode, **Left/Right** on a remote or keyboard and the visible arrow
buttons also move the selected library. Press **OK/Enter**, **Back/Escape**, or
**Done** to finish and return to normal scrolling. Moves are saved immediately;
Back does not undo them. Ordering does not reorder recently added shelves.

**Hide library** removes that library from the home row, its recently added
shelf, and the library switcher without deleting its contents. Choose **Show
hidden libraries** in the menu or beside the home library heading to restore
individual libraries. That control remains available even when every library
is hidden. Order and visibility are saved separately for each account's library
IDs, including while an account is temporarily disconnected.
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
Sprite sheets within the original 50,000,000-byte RGB32 admission envelope
(12.5 megapixels) stay resident: moving between tiles changes only GPU crop
coordinates, without another decode, CPU crop, or texture upload. Ordinary
8-bit YCbCr 4:2:0 JPEGs decode directly into immutable Y/Cb/Cr planes using
Qt's toolchain-selected libjpeg/libjpeg-turbo, with `JDCT_IFAST` by default.
Advanced **Playback → Accurate seek previews** (`playback/accurateTrickplay`)
selects `JDCT_ISLOW` while retaining the same planar path and full encoded
resolution. Changing it revokes the previous generation and decoded cache.
The faster transform is a deliberate IDCT quality tradeoff, not a reduced
resolution or another chroma downsample. Grayscale, 4:2:2, 4:4:4 and other
unsupported JPEG layouts retain Qt's RGB decoder. Oversized sheets retain
codec-side RGB frame clipping. Whole BIF sequences use their real timestamp
indexes and JPEG payloads; no video-player process is used to decode previews.

`TrickplayPreviewItem` integrates with Qt Quick's existing scene graph and the
window's QRhi backend; it does not replace the application renderer or create
a separate GPU context. Qt still owns render scheduling, command submission
and presentation. The item owns three resident compact R8 textures
(RED_OR_ALPHA8 on backends requiring alpha-channel sampling), including on
webOS/GLES. Its QRhi integration uses Qt's private Gui API, so the headers and
runtime must come from the same pinned Qt toolchain on every platform.
The visible quad reconstructs full-range JPEG/601 colour using the source 4:2:0
sampling; odd visible dimensions and raw MCU padding are accounted separately.
Software scene graphs and unsupported GPU texture paths use RGB fallback.
GPU uploads retain immutable backing until the resource-update batch owns it.
No reusable decode pool is added: existing foreground/prefetch caching and
in-flight render/upload ownership are retained rather than overwritten.

The GPU filter is normalized, separable windowed **Lanczos2**:
`sinc(x) * sinc(x/2)` with a fixed two-source-pixel radius. Bilinear pairing of
the two positive middle weights reduces its 4×4 kernel to nine sample
positions (27 compact-plane texture reads for YUV, nine for RGB). Source size,
reciprocal size, chroma geometry and crop bounds are CPU-computed uniforms;
only position-dependent weights are calculated by the fragment shader.
Each tap is clamped to the selected tile's luma pixel centres to avoid
neighbour bleed, while chroma reconstruction retains the whole-JPEG neighbours
used by a full-JPEG RGB decoder. Clamping to the sampled neighbourhood's
colour envelope limits negative-lobe ringing. Work touches only the visible
quad: no full-sheet RGB framebuffer, mipmap chain or resampling pass exists.
Its footprint does not widen at strong minification, deliberately bounding
cost; it is not an ideal scale-adaptive low-pass filter for extreme shrinking.
Software/backend fallback uses that backend's RGB filter, not Lanczos.

**Playback → Seek previews** (`playback/seekPreviews`, default on) controls
thumbnails globally, including the remote-player timeline. Turning it off
immediately hides existing previews, cancels preview fetch/decode work, clears
preview caches, and tells providers to skip preview-only metadata requests.
Turning it on restores the current descriptor when one is available; a provider
that omitted metadata while disabled supplies it on the next playback resolve
or remote-state refresh. The advanced accuracy and size controls are hidden
while previews are off. This preference persists as a device default.

Local and remote previews share a nominal **320 dp** layout width, independent
of encoded thumbnail resolution, preserving the actual source aspect ratio
and fitting the viewport (including the remote page's actual content margins).
The overall interface scale applies, then advanced
**Playback → Seek preview size** (`playback/trickplayPreviewScalePercent`,
25–200%, default 100%) adjusts the display size only. Crop coordinates always
remain in encoded-source pixels. Both settings persist as device defaults
through the ordinary settings schema.

The server-free native fixture smoke exercises real JPEG sprite and BIF pixels,
odd-sized raw 4:2:0 planes with both transforms, grayscale/4:2:2/4:4:4 RGB
fallbacks, accuracy cache retirement, malformed BIF boundaries, resident-sheet
crops, the 50 MB boundary, directional prefetch, and stale-session/late-request
cancellation:

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
and encoded sheets (or one BIF sequence) at 32 MiB. Cache charging is at least
the original RGB32 cost (and includes raw MCU padding when that is larger), so
smaller planes do not expand historical caching. The decoded budget is not
a total RAM/VRAM limit: decoder workspace, active image delivery, optional
software RGB reconstruction and GPU textures add to it. Sequences over 32 MiB
are rejected. The fixture smoke's RGB image readback is a fallback exercise,
not evidence of the production GPU path.

Hover image selection consumes the latest input once per rendered frame;
the timestamp and preview position update immediately. Prefetch follows hover
direction to one neighbouring sheet or at most two BIF frames. Foreground
requests preempt speculative work; a neighbouring sheet is decoded only when
it fits beside the current retained textures, otherwise only its encoded bytes
are prefetched. The preview item bypasses Qt's image cache so historical sheets
do not accumulate there. Switching sessions cancels outstanding delivery and
drops caches.
Remote resource revisions also change when a target switches preview URL or
credentials for the same item. Retired revisions cannot reuse cached frames;
unchanged resources keep stable identities across state polling.

To observe the real renderer during an application preview, set
`QT_LOGGING_RULES='spool.trickplay.render.debug=true'` before launching Spool.
The `planar Lanczos2` texture-format messages identify the compact path; RGB
and software/backend fallbacks are explicitly labelled. `upload` messages
occur on decoded-output replacement, not on same-sheet crop movement.



## Apple TV

Releases include `Spool-<VERSION>-tvOS-arm64.ipa`, an **unsigned** device build
for Apple TV running tvOS 16 or later. It contains the real `Payload/Spool.app`,
but no Apple signing identity or provisioning profile. **tvOS will not install
it as downloaded.** Spool is not distributed through the App Store or TestFlight
and has not been reviewed by Apple. If you already have a tvOS re-signing setup,
use your own certificate, matching entitlements, and provisioning profile; Spool
does not supply a re-signing command or signing credentials.

### Build the device package

Use a Mac with Xcode and the Apple TV SDK installed. Install the build tools and
the matching macOS Qt host tools (the version comes only from the shared pin):

```sh
brew install cmake ninja meson pkg-config bash python imagemagick gpatch
QT_VERSION="$(python3 -c 'import json; print(json.load(open("tools/manifests/toolchain.json"))["qt"]["version"])')"
python3 -m venv build/apple/aqt
build/apple/aqt/bin/pip install aqtinstall
build/apple/aqt/bin/aqt install-qt mac desktop "$QT_VERSION" clang_64 \
  -O "$PWD/build/apple/host-qt" \
  -m qtshadertools qttasktree qtwebsockets qtimageformats
export QT_HOST_PATH="$PWD/build/apple/host-qt/$QT_VERSION/macos"
APPLE_SDK=appletvos APPLE_ARCH=arm64 bash tools/build-tvos.sh
bash tools/package-tvos.sh build/tvos/appletvos-arm64/install/Spool.app dist/tvos
```

The build compiles the pinned source Qt tvOS port and static media dependencies;
a desktop Qt kit is only used for matching host generators, never as a target
SDK. Qt does not list tvOS as an officially supported platform. Target Qt and
media prefixes live under `build/apple/appletvos-arm64/`; the app lives under
`build/tvos/appletvos-arm64/install/`. Apple builds enforce bundled-only loading
and explicit `appleAppStore: true` provider approval; local provider checkout
overrides are not accepted.

The packager performs no signing. It rejects simulator/wrong-architecture
binaries, mismatched bundle versions, signed/provisioned bundles, and unsafe
archive paths before publishing the IPA. Its optional third argument must match
`VERSION`; the embedded bundle versions must match it too.

### Sign and run from source

1. Add your Apple Account in Xcode's account settings, and pair your Apple TV
   with the Mac on the same network. On the TV, open **Settings → Remotes and
   Devices → Remote App and Devices**; use Xcode's **Manage Devices** (or
   **Window → Devices and Simulators** on older Xcode) to pair it.
2. Generate the device project using a unique reverse-DNS identifier you control
   (replace `com.yourname.spool` with your identifier):

   ```sh
   APPLE_SDK=appletvos APPLE_ARCH=arm64 \
     APPLE_BUNDLE_IDENTIFIER=com.yourname.spool bash tools/build-tvos.sh
   open build/tvos/appletvos-arm64/app/SpoolWebOS.xcodeproj
   ```

   The source build configures the app's bundle identifier and matching Keychain
   service together; do not manually replace only the generated plist identifier.
   Select the `spool` application target, not the playback-smoke target.
3. In the application target's **Build Settings**, set **Code Signing Allowed**
   to **Yes**. Under **Signing & Capabilities**, enable **Automatically manage
   signing**, choose your team, and retain the identifier configured above.
4. Select your paired Apple TV and the `spool` scheme, then **Product → Run**.
   Xcode creates the development provisioning profile and signs the app using
   your account. These generated-project changes are local; regenerating the
   project with the unsigned build command resets its build settings.

Once Xcode has configured your signing identity and provisioning profile,
command-line device rebuilds can use `CODE_SIGNING_ALLOWED=YES` and
`APPLE_DEVELOPMENT_TEAM` set to your team ID. Keep `APPLE_BUNDLE_IDENTIFIER`
set to the same identifier. The public release pipeline does not enable this
signing path or obtain credentials from your account.

Follow Apple's current [device signing and run directions](https://developer.apple.com/documentation/xcode/running-your-app-on-simulated-or-physical-devices)
and [device pairing directions](https://developer.apple.com/documentation/xcode/managing-your-simulated-and-physical-devices-in-device-hub).
Account eligibility, provisioning, and signature expiry are Apple's policies.
Simulator success is not a claim of physical-device playback or App Store approval.

The target reuses TV focus, hold-to-open-options, and directional navigation.
Clickpad directions/Select arrive through Qt's UIKit mapping; indirect swipes
step focus without turning the remote into a pointer. Back/Menu dismisses
overlays or navigates back and remains unhandled at the root for tvOS to return
to the launcher. Play/Pause toggles playback. Siri, TV/Home, and volume remain
system-owned. Video pauses when hidden/suspended; audio uses the playback
session and system Now Playing/remote controls. Idle inhibition applies only
while media is active. Account secrets use device-only Keychain records.
Database/artwork/log storage is purgeable on tvOS; local filesystem browsing
and self-updating are not enabled.

### Simulator proof and CI artifacts

```sh
APPLE_SDK=appletvsimulator APPLE_ARCH=arm64 bash tools/build-tvos.sh
bash tools/apple/smoke-tvos.sh build/tvos/appletvsimulator-arm64/install/Spool.app
```

The isolated simulator smoke checks the native app UI, video orientation and
OSD rendering, AudioUnit playback-time advancement and exclusive-session
behavior, plus a real Keychain roundtrip and sandbox file persistence. It writes
`result.json` and `simulator.png` under `build/tvos/smoke/`. The multi-platform
workflow builds both device and simulator on normal branches and PRs under the
existing duplicate-build policy; manual dispatch can select `build_tvos`. Reusable
release builds always include both. Dependency caches are exact-keyed to source
pins, patches, build policy, SDK, architecture, and compiler/build-tool identity;
PRs restore without publishing caches. Only `spool-tvos-device-arm64` is a public
release artifact. Simulator apps and smoke results use
`internal-tvos-simulator-arm64` and are never offered as installable downloads.
Qt owns the GLES framebuffer renderer's external-command bracket; the renderer
does not nest another RHI scope. Context changes restore the provided FBO after
resetting shared OpenGL state, and drawing resets state before Qt resumes.
Attach/detach acknowledgement is delivered on the GUI thread after render-thread
GPU work. Each waiter checks its own completion token, so an old notification
cannot finish a later handoff and stack event-loop receivers stay GUI-owned.
The pixel consumer waits for a real window swap before readback; grabbing an
offscreen frame cannot substitute for native presentation feedback.
Those waits enter Qt's native event loop: on UIKit a manual event pump does not
transfer control to `UIApplicationMain` for normal system presentation.
The identical red/blue fixture is a finite 30-second clip; the consumer does not
reset playback with rapid EOF seeks while renderer initialization is in flight.
Each consumer selector launches a fresh native process, terminating any previous
instance of the harness before dispatching its next argument vector.
The credential consumer atomically writes only its selector, invocation token,
and actual save/load/remove/sandbox-write booleans. The supervisor clears stale
receipts, requires the matching token and all four boolean successes, and
publishes `credentials-result.json` alongside `result.json`; console output is
not the credential acceptance signal.

Simulator builds embed matching local application and Keychain access-group
entitlements in the Mach-O XML/DER sections used by the simulator. Before
installation, the smoke helper ad-hoc signs both bundles with only the host
debug entitlement: putting restricted application entitlements in that macOS
signature prevents launch. This requires no Apple developer certificate,
exercises the real sandboxed Keychain consumer, and does not sign or provision
the separately built device IPA.

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
