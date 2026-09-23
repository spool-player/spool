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
