#!/usr/bin/env bash
# Desktop orchestrator for heaptrack memory profiling of the webOS app.
#
# It arms one-shot profiling on the TV, blocks until the app relaunches under
# the heaptrack preload, records for a fixed window, finalises a clean trace,
# pulls it here, symbolises it on the desktop (heaptrack_interpret), and opens
# the result in heaptrack_gui. tools/webos/package-heaptrack-profile.sh creates
# the profiling IPK; this script is the device and desktop analysis glue.
#
# Symbolisation uses a sysroot assembled from the live process's own
# /proc/<pid>/maps, so we copy exactly the modules that were loaded (small and
# complete) and resolve every frame at its recorded path. The stripped app
# binary is recovered via --extra-paths (build-id matched) from the unstripped
# build output.
set -euo pipefail

HOST="${TV_HOST:-}"
APP_ID="${APP_ID:-com.sachk.spool}"
DURATION="${DURATION:-60}"
OUTDIR=""
OPEN_GUI=1
STOP_AFTER=1
WAIT_TIMEOUT="${WAIT_TIMEOUT:-600}"
LUNA_BIN="${LUNA_BIN:-luna-send}"

usage() {
  cat <<EOF
Usage: tools/webos/profile-memory.sh [options]

Arms heaptrack on the TV, waits for the app to relaunch under profiling,
records DURATION seconds, then pulls + symbolises + opens the trace.

Options:
  --host HOST        required SSH target unless TV_HOST is set
  --duration SEC     record window after profiled launch (default: $DURATION)
  --out DIR          output dir (default: build/memory/heaptrack/<timestamp>)
  --no-stop          leave the app running after recording (partial-but-live trace)
  --no-gui           do not open heaptrack_gui at the end
  --wait-timeout SEC how long to wait for a profiled launch (default: $WAIT_TIMEOUT)
  -h, --help         this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host) HOST="$2"; shift 2 ;;
    --duration) DURATION="$2"; shift 2 ;;
    --out) OUTDIR="$2"; shift 2 ;;
    --no-stop) STOP_AFTER=0; shift ;;
    --no-gui) OPEN_GUI=0; shift ;;
    --wait-timeout) WAIT_TIMEOUT="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done
[[ -n "$HOST" ]] || { echo "missing required --host HOST (or TV_HOST)" >&2; usage >&2; exit 2; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[[ -n "$OUTDIR" ]] || OUTDIR="$ROOT/build/memory/heaptrack/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUTDIR"

SSH=(ssh -F /dev/null -o BatchMode=yes)
SSH_TTY=(ssh -F /dev/null -tt)
MARKER=/tmp/spool-heaptrack.on
CRUMB=/tmp/spool-heaptrack.last

echo "Resolving desktop heaptrack tools from nixpkgs..."
HP="$(nix build nixpkgs#heaptrack --no-link --print-out-paths)"
INTERP="$HP/lib/heaptrack/libexec/heaptrack_interpret"
PRINT="$HP/bin/heaptrack_print"
GUI="$HP/bin/heaptrack_gui"
[[ -x "$INTERP" ]] || { echo "heaptrack_interpret not found at $INTERP" >&2; exit 1; }

# 1. Arm one-shot profiling and clear any stale breadcrumb.
echo "Arming one-shot profiling on $HOST ..."
"${SSH[@]}" "$HOST" "rm -f '$CRUMB'; : > '$MARKER'"

# 2. Block until the shim reports a profiled launch (writes the breadcrumb).
echo "Waiting for the app to relaunch under profiling (timeout ${WAIT_TIMEOUT}s)..."
RAW_REMOTE=""
deadline=$(( $(date +%s) + WAIT_TIMEOUT ))
while :; do
  RAW_REMOTE="$("${SSH[@]}" "$HOST" "cat '$CRUMB' 2>/dev/null" || true)"
  [[ -n "$RAW_REMOTE" ]] && break
  [[ $(date +%s) -ge $deadline ]] && { echo "Timed out waiting for a profiled launch." >&2; exit 1; }
  sleep 2
done
# Enforce one-shot from here (the app user can't delete the root-owned marker in
# sticky /tmp, so the shim's own rm may fail -- we clear it as root instead).
"${SSH[@]}" "$HOST" "rm -f '$MARKER'" 2>/dev/null || true
PID="$("${SSH[@]}" "$HOST" "pidof spool.real 2>/dev/null || pgrep -f spool.real | head -1" || true)"
echo "Profiling live: trace=$RAW_REMOTE pid=${PID:-?}"

# 2b. The +memory-on-stream-start happens at playback start, not app launch.
# Wait for a play event in the app log so the record window brackets it.
if [[ "${WAIT_PLAYBACK:-1}" == "1" ]]; then
  # The app truncates its log on each launch, so a pre-launch line baseline is
  # invalid here (and auto-resume can log "play requested" before we'd sample
  # it). Since the log is fresh for this session, any occurrence is ours.
  LOGF="/tmp/${APP_ID}.log"
  echo "Waiting for playback to start (replay the movie now; falls back after ${PLAY_WAIT:-240}s)..."
  pdeadline=$(( $(date +%s) + ${PLAY_WAIT:-240} ))
  while :; do
    hit="$("${SSH[@]}" "$HOST" "grep -m1 'player: play requested' '$LOGF' 2>/dev/null" || true)"
    [[ -n "$hit" ]] && { echo "Playback started: ${hit#*player: }"; break; }
    [[ $(date +%s) -ge $pdeadline ]] && { echo "No play event seen; recording from now."; break; }
    sleep 2
  done
fi

# 3. Record. Snapshot the loaded module list + RSS breakdown partway through.
echo "Recording ${DURATION}s ..."
sleep $(( DURATION / 2 ))
if [[ -n "$PID" ]]; then
  "${SSH[@]}" "$HOST" "cat /proc/$PID/maps" > "$OUTDIR/maps.txt" 2>/dev/null || true
  "${SSH[@]}" "$HOST" "cat /proc/$PID/smaps_rollup" > "$OUTDIR/smaps_rollup.txt" 2>/dev/null || true
fi
sleep $(( DURATION - DURATION / 2 ))

# 4. Finalise: close the app so the preload flushes a complete trace.
if [[ "$STOP_AFTER" == "1" ]]; then
  echo "Closing app to finalise the trace ..."
  "${SSH_TTY[@]}" "$HOST" "$LUNA_BIN -n 1 luna://com.webos.applicationManager/closeByAppId '{\"id\":\"$APP_ID\"}'" >/dev/null 2>&1 || true
  # Wait for the trace file to stop growing.
  last=-1
  for _ in $(seq 1 30); do
    sz="$("${SSH[@]}" "$HOST" "wc -c < '$RAW_REMOTE' 2>/dev/null" || echo 0)"
    [[ "$sz" == "$last" && "$sz" != "0" ]] && break
    last="$sz"; sleep 1
  done
fi

# 5. Pull the raw trace.
RAW_LOCAL="$OUTDIR/$(basename "$RAW_REMOTE")"
echo "Pulling raw trace -> $RAW_LOCAL"
scp -F /dev/null -q "$HOST:$RAW_REMOTE" "$RAW_LOCAL"

# 6. Build a sysroot from exactly the modules the process had mapped.
SYSROOT="$OUTDIR/sysroot"
mkdir -p "$SYSROOT"
if [[ -s "$OUTDIR/maps.txt" ]]; then
  echo "Assembling sysroot from mapped modules ..."
  mapfile -t mods < <(awk '$6 ~ /^\// {print $6}' "$OUTDIR/maps.txt" | sort -u | sed 's#^/##')
  if [[ ${#mods[@]} -gt 0 ]]; then
    "${SSH[@]}" "$HOST" "tar -C / -cf - ${mods[*]} 2>/dev/null" | tar -C "$SYSROOT" -xf - 2>/dev/null || true
  fi
fi

# 7. Symbolise on the desktop. --sysroot resolves modules by recorded path;
#    --extra-paths recovers the stripped app binary by build-id.
INTERPRETED="$OUTDIR/heaptrack.spool.gz"
EXTRA=()
[[ -f "$ROOT/build/spool.unstripped" ]] && EXTRA+=(--extra-paths "$ROOT/build")
[[ -d "$ROOT/app/lib" ]] && EXTRA+=(--extra-paths "$ROOT/app/lib")
echo "Interpreting trace ..."
# The preload writes an uncompressed raw stream when pointed at a file, but be
# robust to gzip/zstd too.
case "$(od -An -tx1 -N2 "$RAW_LOCAL" | tr -d ' ')" in
  1f8b*) DECOMP=(gzip -dc) ;;
  28b5*) DECOMP=(zstd -dc) ;;
  *)     DECOMP=(cat) ;;
esac
"${DECOMP[@]}" "$RAW_LOCAL" | "$INTERP" --sysroot "$SYSROOT" "${EXTRA[@]}" 2>"$OUTDIR/interpret.log" \
  | gzip -c > "$INTERPRETED"

# 8. Text summary + GUI.
echo "Writing summary ..."
"$PRINT" "$INTERPRETED" > "$OUTDIR/summary.txt" 2>/dev/null || true
echo
echo "================ heaptrack summary ($OUTDIR) ================"
grep -iE "peak heap|peak rss|total runtime|allocations|leaked|temporary" "$OUTDIR/summary.txt" | head -20 || true
echo "============================================================"
echo "Trace:   $INTERPRETED"
echo "Summary: $OUTDIR/summary.txt"
echo "Maps:    $OUTDIR/maps.txt   RSS: $OUTDIR/smaps_rollup.txt"
if [[ "$OPEN_GUI" == "1" && -x "$GUI" ]]; then
  echo "Opening heaptrack_gui ..."
  "$GUI" "$INTERPRETED" >/dev/null 2>&1 &
fi
