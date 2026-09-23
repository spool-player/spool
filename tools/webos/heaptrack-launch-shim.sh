#!/bin/sh
# Spool launch shim (installed in the app bundle as bin/spool).
#
# Default behaviour: a transparent passthrough to the real binary with NO
# environment changes at all -- a normal launch is identical to launching
# spool.real directly. If anything about heaptrack is missing, we
# still just exec the real binary, so profiling can never break playback.
#
# Opt-in profiling: when the marker file exists AND the cross-built heaptrack
# preload is bundled, this run is recorded as a raw heaptrack memory trace via
# LD_PRELOAD. The marker is consumed (one-shot), so only the first launch after
# arming is profiled. The resulting trace path is written to a breadcrumb file
# for the desktop orchestrator. See tools/webos/profile-memory.sh.
set -u

here=$(dirname "$0")
appdir=$(CDPATH= cd -- "$here/.." && pwd)
real="$appdir/bin/spool.real"
preload="$appdir/lib/heaptrack/libheaptrack_preload.so"
marker="${SPOOL_HEAPTRACK_MARKER:-/tmp/spool-heaptrack.on}"
breadcrumb="${SPOOL_HEAPTRACK_BREADCRUMB:-/tmp/spool-heaptrack.last}"

# bigalloc: size-gated allocation tracer. Backtraces only allocations >=
# threshold (default 1 MiB) and periodically dumps the live big-alloc set with
# stacks. Far lower overhead/crash exposure than heaptrack (which unwinds every
# allocation and segfaults here). Marker armed by the operator; not one-shot so
# a relaunch keeps tracing. See tools/webos/bigalloc-report.py.
ba_marker="${SPOOL_BIGALLOC_MARKER:-/tmp/spool-bigalloc.on}"
ba_preload="$appdir/lib/bigalloc.so"
if [ -f "$ba_marker" ] && [ -x "$real" ] && [ -f "$ba_preload" ]; then
    ba_out="/tmp/bigalloc.$$.out"
    echo "$ba_out" > /tmp/spool-bigalloc.last 2>/dev/null || true
    LD_LIBRARY_PATH="$appdir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    QV4_FORCE_INTERPRETER=1 \
    BIGALLOC_OUTPUT="$ba_out" \
    BIGALLOC_THRESHOLD="${SPOOL_BIGALLOC_THRESHOLD:-1048576}" \
    BIGALLOC_DUMP_SEC="${SPOOL_BIGALLOC_DUMP_SEC:-3}" \
    LD_PRELOAD="$ba_preload${LD_PRELOAD:+:$LD_PRELOAD}" \
    exec "$real" "$@"
fi

if [ -f "$marker" ] && [ -x "$real" ] && [ -f "$preload" ]; then
    rm -f "$marker"                       # one-shot: profile only this launch
    out="${SPOOL_HEAPTRACK_OUTPUT:-/tmp/heaptrack.spool.$$.raw}"
    echo "$out" > "$breadcrumb" 2>/dev/null || true
    # The preload's own deps (libunwind, libstdc++) resolve from the app's lib
    # dir and the system. The real binary keeps its own DT_RPATH ($ORIGIN/../lib),
    # which is searched first, so adding this LD_LIBRARY_PATH does not change how
    # the real binary resolves its libraries.
    # Disable Qt QML's V4 JIT while profiling: its generated code has no
    # unwind info, which crashes stack-unwinding backtracers (we saw libunwind
    # _ULarm_step segfault). The interpreter is slower but only used during a
    # capture, and it keeps every frame unwindable.
    LD_LIBRARY_PATH="$appdir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    QV4_FORCE_INTERPRETER=1 \
    LD_PRELOAD="$preload${LD_PRELOAD:+:$LD_PRELOAD}" \
    DUMP_HEAPTRACK_OUTPUT="$out" \
    exec "$real" "$@"
fi

exec "$real" "$@"
