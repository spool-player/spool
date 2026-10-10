# Memory profiling (heaptrack)

We profile the native app's heap with **heaptrack**: every allocation is recorded
with its call stack on the TV, then analysed on the desktop with `heaptrack_gui`
(flame graph of who holds memory, peak-consumption and allocations-over-time
charts, top-allocator tables). This is the tool for "where did the RSS go" — e.g.
the +~350 MB seen on stream start despite only 64 MB of mpv demuxer cache.

Tracy is **not** used for this: it is a frame/CPU/lock profiler and is the right
tool for the playback-pacing work (the `Starfish clock jumped back` issues), not
for breaking down a resident heap footprint.

## How it fits together

- **Device side (recorder only):** a cross-built `libheaptrack_preload.so`. The
  webOS sysroot has no elfutils/libdw, so the device only records a *raw* trace;
  symbolisation happens on the desktop. Runtime deps (libunwind, libstdc++ with
  `CXXABI_1.3.15`) are satisfied by the TV + the app's bundled `libstdc++.so.6.0.33`.
- **Launch shim:** `app/bin/spool` is a tiny shell shim. It is a
  transparent passthrough to `spool.real` **unless** the marker file
  `/tmp/spool-heaptrack.on` exists, in which case it `LD_PRELOAD`s heaptrack
  for that one launch (one-shot) and writes the trace path to
  `/tmp/spool-heaptrack.last`. A failed/absent preload is non-fatal, so normal
  playback can never break.
- **Desktop side (analysis):** `heaptrack_interpret` + `heaptrack_gui` from
  nixpkgs. Symbols are resolved against a sysroot assembled from the live
  process's own `/proc/<pid>/maps` (exactly the loaded modules) plus the
  unstripped `build/spool.unstripped` via `--extra-paths` (build-id
  matched).

## Build and install the recorder

Heaptrack is opt-in and is not included in normal packages. Build and install a
profiling package with an explicit TV host:

```sh
TV_HOST=root@tv.local
BUNDLE_HEAPTRACK=1 ./build-ipk.sh
nix develop -c bash tools/webos/verify-device.sh --no-launch \
  --host "$TV_HOST" ./build/com.sachk.spool_0.3.0_arm.ipk
```

This builds the recorder into `build/webos-heaptrack/install/...` and bundles it
in the IPK. It is built from the same source as the desktop nixpkgs heaptrack,
so the trace format matches.

## Capture a session

Run the orchestrator with the TV host. It arms one-shot profiling, blocks until
the app relaunches under the preload, waits for playback to start, records,
finalises a clean trace, pulls and symbolises it, then opens `heaptrack_gui`:

```sh
tools/webos/profile-memory.sh --host root@tv.local --duration 60
```

Useful options: `--no-stop` (leave the app running; partial-but-live trace),
`--no-gui`, and `--duration SEC`. Set `WAIT_PLAYBACK=0` to record from launch
instead of from the first play event, or `PLAY_WAIT=SEC` to change that wait.

Output lands in `build/memory/heaptrack/<timestamp>/`:
`heaptrack.spool.gz` (open in `heaptrack_gui`), `summary.txt`
(`heaptrack_print`), `maps.txt`, and `smaps_rollup.txt` (RSS breakdown).

## Reading the result for the +350 MB question

- If heaptrack's **peak heap** ≈ the RSS growth, the flame graph names the
  subsystem holding it (ffmpeg per-track demux state, Starfish buffer pools, Qt
  image cache, PGS subtitle bitmaps).
- If heaptrack's tracked **leaked/heap** is much smaller than the RSS growth in
  `smaps_rollup.txt`, the gap is **glibc allocator retention** (freed but not
  returned to the OS) — i.e. arena fragmentation. The app currently runs with no
  `MALLOC_ARENA_MAX`; setting it (or calling `malloc_trim`) is the fix in that case.

## The older coarse tool

`tools/webos/capture-memory.sh` still streams process-level RSS/PSS/swap over time
to a CSV+SVG. It answers "how much" but not "what" — use heaptrack for the breakdown.
