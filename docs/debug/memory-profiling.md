# Performance and memory profiling

## Repeatable native route timings

Use `tools/profile-native.py` for low-overhead measurements of the **real app's**
route construction, GUI CPU time, Qt swap callbacks and frame-budget timer
lateness. It drives the existing `RenderBenchmark`/`InputLatencyMonitor`, not a
second timing implementation. No instrumentation runs in normal app launches.
`spoolet` is a navigation/control CLI; it does not currently expose a performance
sampling command.

Build the same optimized binary for every comparison:

```sh
nix run .#build
nix develop .#native -c python3 tools/profile-native.py \
  --app build/linux-release/install/bin/spool \
  --output build/profiles/before
```

The driver is Linux-only and always uses the existing private Xvfb + Weston
display with pinned Mesa llvmpipe/OpenGL, never your desktop or a TV. Every run
has a fresh empty profile, private session bus and temporary caches. Inherited
Spool data/cache/credential/benchmark overrides are discarded. The runner does
not attach to an existing instance or restore accounts. This workload measures
empty-shell route transitions, **not** provider response processing, populated
library scrolling, playback, physical GPU speed, panel presentation or heap/RSS.
The native development shell supplies the pinned graphics environment and
display tools; running outside it fails with an actionable error.

By default it performs three fresh app/display runs in each mode, with one
unmeasured warmup walk and four measured walks per run:

- `warm`: app pages remain resident after warmup.
- `eviction`: the existing benchmark requests application memory-pressure
  eviction before transitions. This is **not** a cold OS page cache, Qt disk/AOT
  cache or newly started process for every transition.

The output directory must be new; existing results are never overwritten.
Artifacts are private (directory `0700`, files `0600`):

- `warm-01.json`, `eviction-01.json`, etc.: the app's actual schema-2 reports.
- `summary.json`: completion/failure status, SHA-256 of the measured executable,
  workload configuration, rendering environment, per-route medians/sample
  counts/worst timer lateness and each run's idle timer noise floor.
- `summary.md`: median/min/max overall time across fresh runs. An overall time
  is the **sum of per-route median wall times within one run**; startup and idle
  settle delays are not included. Range across repeats exposes shared-machine
  variability, not a statistical confidence interval.

No raw application logs, support bundles, settings, account stores, environment
dumps or screenshots are retained or printed. Temporary state is removed on
success, failure and cancellation. Each run has a total `--timeout` (default
120 seconds, including display startup) plus `--step-timeout-ms` (default 8000).
Ctrl-C/SIGTERM stops only that run's owned app/compositor process group, with
a bounded three-second grace before forced termination. Missing/invalid metrics,
missing route steps, incomplete reports, changing rendering environments and
nonzero app exits fail the command; `summary.json` remains `complete: false`.
No failure report should be treated as a performance result.

After building the changed revision, repeat with a **different** output
directory. Compare matching per-run reports on the same idle machine:

```sh
nix develop .#native -c python3 tools/profile-native.py \
  --app build/linux-release/install/bin/spool --output build/profiles/after
python3 tools/compare-render-benchmark.py build/profiles/after/warm-01.json \
  --baseline build/profiles/before/warm-01.json --require-comparable \
  --markdown build/profiles/after/warm-comparison.md
```

`--require-comparable` refuses absent baselines or differences in workload,
Qt/provider runtime, timing origin or rendering environment, even with
`--warn-only`. Without it, the report helper explicitly labels an absent or
incompatible baseline and prints measurements without deltas (useful for CI's
first run). Missing/non-finite metrics and empty/incomplete current or baseline
reports always fail. Avoid conclusions from one pair of runs: examine all
repeats and idle noise. Timer lateness is **not** dropped frames; swap callbacks
are **not** display presentation.

### Small isolated smoke

After a native release build, this exercises both modes without user state:

```sh
nix develop .#native -c python3 tools/profile-native.py \
  --app build/linux-release/install/bin/spool \
  --output build/profiles/smoke --repeats 1 --iterations 1 --timeout 90
python3 tools/compare-render-benchmark.py build/profiles/smoke/warm-01.json
```

Expect exit zero, `summary.json` with `complete: true`, one report per mode and
nonempty measured route samples. A separate invocation with a new output
directory and `--timeout 0.1` must exit nonzero with `complete: false`, not leave
an app/compositor running or reuse the successful smoke's artifacts. For a
cancellation smoke, start the normal command with another new output directory,
press Ctrl-C, and inspect only its `summary.json` completion/failure fields.

For allocation call stacks instead of route timings, use heaptrack below.
For native symbol-based CPU/allocation investigations use the existing
`linux-dev` CMake preset (`RelWithDebInfo`, development features enabled); do
not compare it directly to `linux-release` (`Release`, development features
disabled). Neither preset alone records a profile.

## Heap allocation profiling (heaptrack)

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

Run device commands only with explicit permission for that TV. Installation is
install-only; arming a capture does not launch the app. A profiling session must
be launched separately by the user, and the default capture closes that app to
flush the trace.

```sh
TV_HOST=root@tv.local
BUNDLE_HEAPTRACK=1 ./build-ipk.sh
nix develop -c bash tools/webos/verify-device.sh --no-launch \
  --host "$TV_HOST" "./build/com.sachk.spool_$(cat VERSION)_arm.ipk"
```

This builds the recorder into `build/webos-heaptrack/install/...` and bundles it
in the IPK. It is built from the same source as the desktop nixpkgs heaptrack,
so the trace format matches.

The dedicated `tools/webos/package-heaptrack-profile.sh` helper creates its
profiling package from a temporary staging copy, preserving the canonical
release stage. Its `--benchmark-only` mode omits heaptrack and packages only the
benchmark-aware launch shim; that package supports route benchmarks, **not**
heap allocation recording. Helper artifacts remain under `build/webos-profile`.

## Capture a session

Run the orchestrator with the TV host. It arms one-shot profiling, waits (up to
`--wait-timeout`, default 600 seconds) for a separately requested app launch under
the preload, waits for playback, records, finalises a trace, pulls and symbolises
it, then writes a text summary. No desktop GUI opens by default:

```sh
tools/webos/profile-memory.sh --host root@tv.local --duration 60
```

Useful options: `--no-stop` (leave the app running; partial-but-live trace),
`--out DIR`, `--wait-timeout SEC`, `--duration SEC`, and `--gui` (explicitly
open `heaptrack_gui`; `--no-gui` remains accepted). Set `WAIT_PLAYBACK=0` to record
from launch instead of from the first play event, or `PLAY_WAIT=SEC` (default
240) to change the playback wait. If no play event is seen before that deadline,
the current helper records anyway: do not label that trace a measured
playback-start allocation profile without independently confirming playback.

Output lands in `build/memory/heaptrack/<timestamp>/`:
`heaptrack.spool.gz` (open in `heaptrack_gui`), `summary.txt`
(`heaptrack_print`), `maps.txt`, and `smaps_rollup.txt` (RSS breakdown).

Unlike the isolated native timing artifacts, these traces, mapping paths,
symbolisation logs and collected module files are private device evidence.
Keep them local; share only the minimum filtered numeric summary or redacted
call stacks needed for diagnosis, never unfiltered app logs/support bundles.

## Reading the result for the +350 MB question

- If heaptrack's **peak heap** ≈ the RSS growth, the flame graph names the
  subsystem holding it (ffmpeg per-track demux state, Starfish buffer pools, Qt
  image cache, PGS subtitle bitmaps).
- If heaptrack's tracked heap is much smaller than the RSS growth in
  `smaps_rollup.txt`, investigate allocator retention/fragmentation **and**
  memory heaptrack cannot see (mapped files, driver/video allocations and other
  non-heap mappings). RSS minus tracked heap is not by itself proof of glibc
  retention. Compare mapping categories and allocation lifetimes before trying
  allocator tuning such as `MALLOC_ARENA_MAX`.

## The older coarse tool

`tools/webos/capture-memory.sh` streams process-level RSS/PSS/swap over time to
a CSV+SVG. It defaults to a bounded `--duration 300` seconds, rejects a zero
duration, and bounds the wait for the target process with `--wait-timeout 600`
seconds. It answers "how much" but not "what" — use heaptrack for the breakdown.
