# Spool for Jellyfin

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
   `gpu-api=opengl`, `gpu-context=auto`, `wid=-1`, `force-window=no`, `idle=yes`,
   `keep-open=no`, `input-vo-keyboard=no`, `input-cursor=no`, `terminal=no` and
   `osc=no`. These are enforced before mpv initializes input, scripts or output.
   Spool also owns the initial window/fullscreen state, authenticated media
   requests, TLS verification, resume position, SyncPlay and explicit track
   restoration.

This is still embedded OpenGL playback, not native Vulkan, D3D11 or Metal
presentation, and it does not enable desktop HDR output. Native `gpu-context`
names such as `winvk` or `d3d11` are not mapped to embedded backends. Scripts and
dynamic profiles must not change the embedding options; native-window commands,
renderer replacement and standalone mpv playlist management are unsupported.
Config errors and embedding ownership are reported in player/mpv diagnostics.

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

Automatic picture-quality adjustment restarts an active stream at its saved
position, retaining pause, selected tracks and per-file sync delays. On Android,
the ladder can ultimately switch from Enhanced to Direct MediaCodec output.
Handset 100% uses a calibrated dp baseline; card counts follow available width
and zoom rather than a fixed phone grid. The Android Qt build carries a
live-density notification patch so fold/configuration changes update the UI
without requiring an app restart.

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

## Fast Jellyfin client for LG TVs and desktop

// Download link box

// quick headline Features dot points

// links to below sections

// screenshots

// full feature list

// usage information

// technical information

// thank yous (kodi)
