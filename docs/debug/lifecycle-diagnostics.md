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

- `tools/webos/diagnose.sh root@tv.local`
- `tools/webos/collect-diagnostics-bundle.sh root@tv.local`
- `tools/webos/kill-stale.sh root@tv.local`

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
surface and follow the existing `mpv-video-item*` GPU-test exclusion in headless
CI; `offscreen` alone does not verify compositor configure/presentation behavior.

To measure without opening a window on the user's desktop, start an isolated
headless compositor (for example `nix-shell -p weston --run
'weston --backend=headless --renderer=gl --width=1920 --height=1080
--socket=spool-fullscreen-test --no-config --idle-time=0'`), then run the built
tests from another shell:

```sh
nix develop .#native -c env QT_QPA_PLATFORM=wayland WAYLAND_DISPLAY=spool-fullscreen-test \
  ctest --test-dir build/linux-dev/app -R '^mpv-video-item-fullscreen' -V
```

A headless compositor exercises actual Wayland configuration, swapchain resize,
and embedded video rendering, but not the user's compositor animations, physical
display presentation, or HDR output. Those require a separate hardware check.
