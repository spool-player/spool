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

- `tools/webos/diagnose.sh root@tv.local` prints allowlisted instance metadata,
  never raw logs, process command lines, or diagnostic paths.
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
