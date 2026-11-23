# Lifecycle Diagnostics

Build with `-DSPOOL_DIAGNOSTICS=ON` to enable debug-only evidence for stale process and slow relaunch issues.

Optional flags:

- `-DSPOOL_DIAGNOSTICS_STACKDUMP=ON` requests `gdb` stack dumps when available.
- `-DSPOOL_DIAGNOSTICS_ABORT_ON_HANG=ON` aborts after watchdog evidence is written.

Generated files:

- `current-instance.json`: current pid, state, instance id, uptime, and diagnostics root.
- `lifecycle.jsonl`: startup, shutdown, task, thread, network, and signal events.
- `watchdog/watchdog.jsonl`: GUI event-loop and shutdown watchdog events.
- `stale-processes.json`: previous heartbeat plus matching `/proc` process snapshots.
- `proc/*.json`: self and matching process `/proc` snapshots.
- `stackdump/*.gdb.txt`: optional gdb thread backtraces.

webOS helpers:

- `tools/webos/diagnose.sh root@tv.local` discovers the running executable's
  process environment and derives the current Qt app-data root
  (`$XDG_DATA_HOME/spool/Spool`, or `$HOME/.local/share/spool/Spool`).
  It honors the process's `SPOOL_DIAGNOSTICS_DIR`, reads only
  `diagnostics/current-instance.json`, and prints allowlisted numeric/state
  metadata, never raw logs, process command lines, environment, or paths.
  A normal build without lifecycle diagnostics reports the running PID and
  `metadataAvailable: false`; missing metadata is not a failed discovery.
- `tools/webos/collect-diagnostics-bundle.sh root@tv.local` streams a read-only
  archive to the desktop. It contains `ps.txt` and a nested `logs.tar.gz`; no
  temporary directories or archives are created on the TV. Treat logs as private.
- `tools/webos/kill-stale.sh root@tv.local` explicitly terminates processes whose
  executable belongs to this app bundle, rechecking before forced termination.

These commands require an explicit target, validate `APP_ID`, and use
noninteractive SSH with connection/keepalive limits and a per-command
`WEBOS_COMMAND_TIMEOUT` (30 seconds by default). They never restart services,
change app data ownership, or patch installed files. Do not run device helpers
unless device diagnosis/deployment was explicitly requested.

### Remote spoolet on webOS

The packaged app includes the authenticated control server, but webOS never
starts it by default. Explicitly opt in on a **fresh normal app launch**:

```sh
ares-launch --device tv --params \
  '{"SPOOL_AUTOMATION_PORT":"0","SPOOL_AUTOMATION_INSTANCE":"streaming-measure"}' \
  com.sachk.spool
```

The equivalent application-manager launch payload is
`{"id":"com.sachk.spool","params":{"SPOOL_AUTOMATION_PORT":"0","SPOOL_AUTOMATION_INSTANCE":"streaming-measure"}}`.
If Spool is already running, close it normally first: this is a launch-only
opt-in, not a relaunch command handler. `SPOOL_AUTOMATION_PORT` accepts
`0..65535` (zero selects a free port); direct executable launches also accept
`--automation-port PORT` and `--instance ID`. An invalid explicit port or
instance fails startup rather than silently disabling the requested endpoint.
The listener always binds **127.0.0.1**, never a LAN address. Mobile/webOS
launches without the explicit port have no control listener.

Run the desktop CLI; nothing needs to be installed separately on the TV:

```sh
spoolet --host root@192.168.0.200 instances
spoolet --host root@192.168.0.200 --instance streaming-measure status
spoolet --host root@192.168.0.200 --instance streaming-measure items libraries
spoolet --host root@192.168.0.200 --instance streaming-measure library QUALIFIED_ID
spoolet --host root@192.168.0.200 --instance streaming-measure items browse
spoolet --host root@192.168.0.200 --instance streaming-measure play QUALIFIED_ITEM_ID
spoolet --host root@192.168.0.200 --instance streaming-measure settings set playback/threadedStreaming true
spoolet --host root@192.168.0.200 --instance streaming-measure settings set playback/maxStreamingThreads 4
spoolet --host root@192.168.0.200 --instance streaming-measure settings set playback/forceStreamingThreads false
spoolet --host root@192.168.0.200 --instance streaming-measure settings set playback/logStreamingPerformance true
spoolet --host root@192.168.0.200 --instance streaming-measure stop
spoolet --host root@192.168.0.200 --instance streaming-measure speed-test
spoolet --host root@192.168.0.200 --instance streaming-measure speed-test status
```

Speed tests are asynchronous and defer while playback is active. Poll
`speed-test status` until each account has neither `pending` nor `running`.
The response exposes only provider module IDs, opaque account IDs, flags, a
numeric measured quality limit (`measuredMbps`, including the normal safety
margin), selected logical request counts, and the per-request throughput curve
(`laneMbps`, raw Mbps). It never returns account names, endpoint URLs, tokens,
headers, or provider configuration. Actual playback remains separate evidence:
poll `status` for playback progress and numeric network metrics.

`tools/profile-streaming.py --spoolet PATH --instance ID --item QUALIFIED_ID
--output NEW_PRIVATE_DIR --request-counts 1,4,auto --repeats 5 --seconds 60
--from-start` compares counts in paired repeat order; add `--host USER@HOST`
for a remote instance. It refuses existing playback and restores changed
streaming controls in `finally`. Schema-3 reports keep explicit bounded numeric
transport fields and protocol identities. Ordered/received rates omit intervals
whose endpoints are paused or seeking, but include full-cache idle time:
they are observed playback delivery rates, not isolated network capacity.
`countReached=false` invalidates a fixed-count comparison; it is not a slower
result for that count. HTTP/protocol and receive-window controls are only
available when their transport consumers ship.

SSH must already have a trusted host key and noninteractive authentication.
The CLI uses `BatchMode=yes`, strict host-key checking, no SSH configuration or
connection sharing, and `ssh -W` encrypted stdio forwarding directly to the TV's
loopback port. It creates no local forwarding listener, remote temporary file,
or persistent tunnel. All child processes and response sizes are bounded;
timeouts kill and reap the SSH child. `--timeout MS` (100..30000, default
10000) bounds remote discovery, probing, and the command together. SSH stderr
is not copied into CLI output, and the capability is sent only through stdin,
never a process argument or printed descriptor.

Discovery uses the same process-derived Qt root as `diagnose.sh`, under
`runtime/spool-control`. Descriptor directories must be owner-only (0700),
files must be owner-only (0600), regular and not symlinks. The directory and
file owners must match the packaged app process UID. Non-root SSH can discover
only its own UID; root SSH can read the app-owned private capability without
changing sandbox ownership. An explicit descriptor selected by root must
likewise have matching private directory/file owners. `--instance ID` selects one discovered instance;
without it a command requires exactly one reachable instance. With `--host`,
`--descriptor /absolute/path.json` means an explicit **remote** private
descriptor, not a downloaded local file. Without `--host`, it retains its
local meaning. `--descriptor` and `--instance` are mutually exclusive.
Do not print or copy descriptor contents. A normal restart without the launch
opt-in disables remote control; clean shutdown removes the descriptor.

### Package and verification modes

`./build-ipk.sh` and `./build-ipk.sh all` both run the complete pipeline:
verified source fetch, host Qt, target Qt, dependencies, fresh app configure,
staging/audits, then packaging. Phase entry points exist for CI orchestration;
do not use them as shortcuts when deploying. Help and invalid arguments never
enter Nix, restore a toolchain, download sources, or touch a device.

`tools/webos/verify-device.sh --host root@tv.local` installs the newest matching
IPK by modification time and verifies exact app registration. Its default is
install-only; `--no-launch` makes that intent explicit. `--no-install` reads
registration without requiring a package. Only `--launch` launches the app,
collects logs, and captures a screenshot; `--skip-screenshot` omits that capture.
LS2 service errors, missing registration and failed capture fail the command.
Installation subscriptions are bounded by `--install-timeout` (120 seconds).
Screenshot files created in `/tmp` are removed after retrieval.

### Device render benchmark

Build `tools/webos/package-heaptrack-profile.sh --benchmark-only` and explicitly
install its IPK from `build/webos-profile` before running
`tools/webos/bench-device.sh --host root@tv.local`. The ordinary heaptrack
profiling IPK also supports benchmarks. Instrumentation is added to a separate
local staging copy, never the canonical release stage or installed application.

The benchmark explicitly closes and launches only Spool, supplies data through
a temporary `/tmp` configuration read by the packaged shim, retrieves and
validates the report, then removes its temporary files and closes Spool. It
never rewrites `bin/spool`, changes app data ownership, wakes the panel, or
switches TV inputs. The panel must already be active; a sleeping panel or
incomplete/invalid report fails the measurement. `--wait` bounds report polling
(240 seconds by default), and each transport command is independently bounded.

Simulation knobs:

- `SPOOL_DIAGNOSTICS_DIR=/tmp/com.sachk.spool-diagnostics` overrides the output directory.
- `SPOOL_DIAGNOSTICS_BLOCK_GUI_MS=8000` blocks the GUI thread after startup to test watchdog capture.
- `SPOOL_DIAGNOSTICS_SHUTDOWN_HANG_MS=8000` blocks shutdown to test shutdown-stall capture.

Typical stale-process flow:

1. Install a diagnostics build.
2. Launch, wait for home, close, then relaunch after the slow/no-op symptom.
3. Run `tools/webos/collect-diagnostics-bundle.sh root@tv.local`.
4. Inspect `stale-processes.json`, `current-instance.json`, `lifecycle.jsonl`, and `watchdog/watchdog.jsonl` first.

## Desktop Wayland fullscreen

Native fullscreen requests must not synchronously set mpv's `fullscreen`
property on the GUI thread. Qt's threaded scene graph depends on that thread
for synchronization and queued video updates; mpv may wait for the scene graph
to render or report a swap. The resulting circular wait ends at mpv's VO timeout,
not when the compositor configures the window. Queue the property change and
renew its observation after the latest reply so stale echoes cannot reverse a
newer window request. Custom mpv fullscreen bindings still control the native
window. No timer, forced redraw loop, SDR fallback, or surface recreation is
needed.

Runtime color-diagnostic snapshots have the same dependency and are collected
on the existing mpv event thread, using that event loop's lifetime-owned handle.
They read no GUI state and retain the same color/HDR properties; a playback
restart can no longer hold up a simultaneous window resize to collect them.

The `mpv-video-item-fullscreen` and `mpv-video-item-fullscreen-vulkan` consumer
tests exercise native enter/exit while idle, playing, paused, and stopped, plus
rapid requests and custom mpv bindings. Each transition prints the synchronous
call time and time to a resized swapped frame. They require a real graphics
surface and run in the unified GUI e2e phase on an isolated Weston/Xvfb display
with Mesa CPU GL/Vulkan drivers; `offscreen` alone does not verify compositor
configure/presentation behavior.

To measure without opening a window on the user's desktop, use the same private
compositor wrapper as CI:

```sh
nix develop .#native -c bash tools/test-gpu-session.sh \
  build/linux-dev/app/spool-e2e-tests --child mpv-video-item-fullscreen
nix develop .#native -c bash tools/test-gpu-session.sh \
  build/linux-dev/app/spool-e2e-tests --child mpv-video-item-fullscreen-vulkan
```

A headless compositor exercises actual Wayland configuration, swapchain resize,
and embedded video rendering, but not the user's compositor animations, physical
display presentation, or HDR output. Those require a separate hardware check.

For repeatable whole-app route timing instead of fullscreen-specific timings,
see [native route profiling](memory-profiling.md#repeatable-native-route-timings).
That driver reuses the private display, isolates app state for every repeat and
retains validated JSON plus aggregate summaries without raw application logs.
