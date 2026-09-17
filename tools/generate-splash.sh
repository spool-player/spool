#!/usr/bin/env bash
# Render the launch screen for one platform.
#
# The mark, the wordmark and the version are one block -- the "core" -- drawn
# on black. Everything that puts a launch screen on screen draws that same
# block centred, so the frame the operating system shows and the first frame
# Qt paints are the same picture and the handover is invisible.
#
#   tools/generate-splash.sh --platform desktop|webos|android \
#                            --out DIR [--version X.Y.Z]
#
# The version is part of the picture, so this runs per build rather than
# leaving a rendered PNG in the tree to go stale.
set -euo pipefail

APP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="$APP_ROOT/tools/manifests/splash.json"
PLATFORM=""
OUT_DIR=""
VERSION=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --platform) PLATFORM="$2"; shift 2 ;;
    --out) OUT_DIR="$2"; shift 2 ;;
    --version) VERSION="$2"; shift 2 ;;
    *) echo "error: unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ -n "$PLATFORM" ]] || { echo "error: --platform is required" >&2; exit 2; }
[[ -n "$OUT_DIR" ]] || { echo "error: --out is required" >&2; exit 2; }
[[ -n "$VERSION" ]] || VERSION="$(tr -d '[:space:]' <"$APP_ROOT/VERSION")"

case "$PLATFORM" in
  desktop | webos | android) ;;
  *) echo "error: unsupported splash platform: $PLATFORM" >&2; exit 2 ;;
esac

# Same self-bootstrap as the icon generator: a machine without ImageMagick
# borrows one rather than making the caller install it first. A flakes-only Nix
# has nix-shell on PATH but no <nixpkgs> to give it, so check for the channel
# and not just the command -- otherwise the useful error below is replaced by a
# Nix evaluation trace that says nothing about launch screens.
if ! command -v magick >/dev/null 2>&1; then
  if command -v nix-shell >/dev/null 2>&1 &&
    nix-instantiate --find-file nixpkgs >/dev/null 2>&1; then
    quoted=()
    for arg in "$0" --platform "$PLATFORM" --out "$OUT_DIR" --version "$VERSION"; do
      printf -v one '%q' "$arg"
      quoted+=("$one")
    done
    exec nix-shell -p imagemagick --run "exec bash ${quoted[*]}"
  fi
  echo "error: ImageMagick (magick) is required to render the launch screen" >&2
  exit 1
fi

# The interpreter is resolved by the shared manifest library: Windows ships it
# as python only, every other platform here has python3.
# shellcheck source=tools/lib/manifest-sources.sh
source "$APP_ROOT/tools/lib/manifest-sources.sh"

field() {
  "$MANIFEST_PYTHON" -c '
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    value = json.load(handle)
for key in sys.argv[2].split("."):
    value = value[key]
print(value)
' "$MANIFEST" "$1"
}

SCALE="$(field renderScale)"
CORE_W="$(field core.width)"
CORE_H="$(field core.height)"
CORE_BG="$(field core.background)"
LOGO_SIZE="$(field logo.size)"
LOGO_OFFSET_Y="$(field logo.offsetY)"
WORDMARK_TEXT="$(field wordmark.text)"
WORDMARK_SIZE="$(field wordmark.pointSize)"
WORDMARK_WEIGHT="$(field wordmark.weight)"
WORDMARK_COLOR="$(field wordmark.color)"
WORDMARK_OFFSET_Y="$(field wordmark.offsetY)"
VERSION_SIZE="$(field version.pointSize)"
VERSION_WEIGHT="$(field version.weight)"
VERSION_COLOR="$(field version.color)"
VERSION_OFFSET_Y="$(field version.offsetY)"
VERSION_PREFIX="$(field version.prefix)"
REFERENCE_W="$(field reference.width)"
REFERENCE_H="$(field reference.height)"
ANDROID_SOURCE_W="$(field android.systemIconSourceWidth)"
ANDROID_CORE_BUCKET="$(field android.coreDensityBucket)"
ANDROID_ICON_CANVAS_DP="$(field android.systemIconCanvasDp)"

scaled() { echo $(($1 * SCALE)); }

FONT="$APP_ROOT/qml/fonts/IBMPlexSans-Variable.ttf"
LOGO_SVG="$APP_ROOT/app/icons/spool.svg"

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT
mkdir -p "$OUT_DIR"

# The mark is vector art; rasterise it once at the supersampled size and let
# the single downsample at the end do all the filtering.
logo="$tmp_dir/logo.png"
magick -background none -density 4096 "$LOGO_SVG" \
  -resize "$(scaled "$LOGO_SIZE")x$(scaled "$LOGO_SIZE")"\! "$logo"

render_core() {
  local width="$1" height="$2" destination="$3"
  magick -size "$(scaled "$CORE_W")x$(scaled "$CORE_H")" xc:"$CORE_BG" \
    "$logo" -gravity center -geometry "+0$(printf '%+d' "$(scaled "$LOGO_OFFSET_Y")")" -composite \
    -font "$FONT" -gravity center \
    -pointsize "$(scaled "$WORDMARK_SIZE")" -weight "$WORDMARK_WEIGHT" -fill "$WORDMARK_COLOR" \
    -annotate "+0$(printf '%+d' "$(scaled "$WORDMARK_OFFSET_Y")")" "$WORDMARK_TEXT" \
    -pointsize "$(scaled "$VERSION_SIZE")" -weight "$VERSION_WEIGHT" -fill "$VERSION_COLOR" \
    -annotate "+0$(printf '%+d' "$(scaled "$VERSION_OFFSET_Y")")" "$VERSION_PREFIX$VERSION" \
    -colorspace RGB -filter Lanczos -resize "${width}x${height}"\! \
    -colorspace sRGB -depth 8 -strip -define png:color-type=2 "$destination"
}

case "$PLATFORM" in
  desktop)
    # Qt scales this to the viewport, so ship it at the size the largest
    # screen asks for rather than letting it be enlarged.
    render_core "$ANDROID_SOURCE_W" "$((ANDROID_SOURCE_W * CORE_H / CORE_W))" "$OUT_DIR/splash-core.png"
    ;;
  webos)
    render_core "$ANDROID_SOURCE_W" "$((ANDROID_SOURCE_W * CORE_H / CORE_W))" "$OUT_DIR/splash-core.png"
    # webOS names a full-screen background in appinfo.json, and the panel is
    # always 16:9 1080p, so the whole frame can be rendered exactly.
    reference_core="$tmp_dir/core-reference.png"
    render_core "$CORE_W" "$CORE_H" "$reference_core"
    magick -size "${REFERENCE_W}x${REFERENCE_H}" xc:"$CORE_BG" \
      "$reference_core" -gravity center -composite \
      -depth 8 -strip -define png:color-type=2 "$OUT_DIR/splash.png"
    ;;
  android)
    # One package serves both form factors, so both launch screens ship and
    # the platform picks between them with the -television qualifier. The
    # qualifier sorts before density, so a television at xhdpi still resolves
    # the television bitmap rather than the handset one a density nearer.
    mkdir -p "$OUT_DIR/res/drawable" \
      "$OUT_DIR/res/drawable-xxxhdpi" "$OUT_DIR/res/drawable-nodpi" \
      "$OUT_DIR/res/drawable-television-xxxhdpi" "$OUT_DIR/res/drawable-television-nodpi"

    render_android_form_factor() {
      local core_dp="$1" res_suffix="$2" label="$3"
      # A bucketed bitmap is scaled by density/bucket, so its dp size is exact.
      # A nodpi one is drawn at its natural pixel size and the layer-list's
      # android:width is ignored, which is what drew the mark half again too big.
      local core_bucket_px=$((core_dp * ANDROID_CORE_BUCKET / 160))
      local core="$OUT_DIR/res/drawable$res_suffix-xxxhdpi/spool_splash_core.png"
      render_core "$core_bucket_px" "$((core_bucket_px * CORE_H / CORE_W))" "$core"

      # From API 31 the platform draws its own launch icon, centred, into a box
      # of systemIconCanvasDp, before the activity exists -- so it, not the
      # window background, is the first frame. Left alone it uses the launcher
      # icon, which is a square bitmap and reads as a boxed icon on black.
      # Draw the mark into that box at exactly the size the core gives it, on
      # transparency, so the platform's frame and Qt's put the same mark at the
      # same size in the same place and only the words appear afterwards.
      local icon="$OUT_DIR/res/drawable$res_suffix-nodpi/spool_splash_icon.png"
      local icon_canvas_px=$((ANDROID_SOURCE_W))
      local mark_dp=$((core_dp * LOGO_SIZE / CORE_W))
      local mark_px=$((icon_canvas_px * mark_dp / ANDROID_ICON_CANVAS_DP))
      magick -size "${icon_canvas_px}x${icon_canvas_px}" xc:none \
        \( -background none -density 4096 "$LOGO_SVG" -resize "${mark_px}x${mark_px}"\! \) \
        -gravity center -composite \
        -depth 8 -strip "$icon"

      # Measured, not assumed: the two frames only agree if these agree. The
      # mark is the only bright thing in the top half of either image -- the
      # words sit below centre -- so the trim box of that half is its diameter.
      local core_mark_px icon_mark_px
      core_mark_px="$(mark_width "$core")"
      icon_mark_px="$(mark_width "$icon")"
      printf '  %s window background: mark %ddp\n' \
        "$label" "$((core_mark_px * core_dp / core_bucket_px))"
      printf '  %s system launch icon: mark %ddp as the platform will draw it\n' \
        "$label" "$((icon_mark_px * ANDROID_ICON_CANVAS_DP / ANDROID_SOURCE_W))"
    }

    mark_width() {
      magick "$1" -crop 100%x50%+0+0 +repage -threshold 39% -format '%@' info: | sed 's/x.*//'
    }

    phone_dp="$(field android.phoneCoreWidthDp)"
    tv_dp="$(field android.tvCoreWidthDp)"
    render_android_form_factor "$phone_dp" "" "handset"
    render_android_form_factor "$tv_dp" "-television" "television"

    # Qt scales its own copy to whichever dp the running device asked for, so
    # one bitmap serves both; render it at the larger of the two so the
    # television only ever scales down.
    larger_dp="$phone_dp"
    ((tv_dp > larger_dp)) && larger_dp="$tv_dp"
    larger_px=$((larger_dp * ANDROID_CORE_BUCKET / 160))
    render_core "$larger_px" "$((larger_px * CORE_H / CORE_W))" "$OUT_DIR/splash-core.png"

    # Black everywhere, the core centred at an explicit dp size. Qt reads the
    # same dp out of the manifest, so its first frame lands on the same
    # pixels this one did and the handover shows nothing. One layer-list
    # serves both: @drawable/spool_splash_core resolves per form factor.
    cat >"$OUT_DIR/res/drawable/spool_splash.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<!-- Generated by tools/generate-splash.sh; do not edit. -->
<layer-list xmlns:android="http://schemas.android.com/apk/res/android">
    <item android:drawable="@android:color/black" />
    <item>
        <bitmap android:src="@drawable/spool_splash_core" android:gravity="center" />
    </item>
</layer-list>
XML
    ;;
esac

printf 'launch screen for %s %s -> %s\n' "$PLATFORM" "$VERSION" "$OUT_DIR"
