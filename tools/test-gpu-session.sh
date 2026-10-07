#!/usr/bin/env bash
# Run GUI consumers on a private display, never the developer's desktop.
set -euo pipefail
[[ $# -gt 0 ]] || { echo 'usage: test-gpu-session.sh <command> [args...]' >&2; exit 2; }
: "${SPOOL_TEST_DRI_DIR:?run through nix develop .#native}"
: "${SPOOL_TEST_VULKAN_ICD:?run through nix develop .#native}"
: "${SPOOL_TEST_DRIVER_LIB_DIR:?run through nix develop .#native}"
: "${SPOOL_TEST_EGL_VENDOR:?run through nix develop .#native}"
[[ -d "$SPOOL_TEST_DRI_DIR" && -f "$SPOOL_TEST_VULKAN_ICD" ]] || {
  echo 'error: pinned Mesa GL/Vulkan CPU drivers are missing' >&2
  exit 1
}
work="$(mktemp -d -t spool-gpu-tests.XXXXXXXX)"
display_pid=''
cleanup() {
  if [[ -n "$display_pid" ]]; then
    kill "$display_pid" 2>/dev/null || true
    wait "$display_pid" 2>/dev/null || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$work/runtime"
chmod 700 "$work/runtime"
export XDG_RUNTIME_DIR="$work/runtime"
export SPOOL_E2E_ISOLATED_DISPLAY=1
unset DISPLAY WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS QT_QUICK_BACKEND
unset VK_ICD_FILENAMES VK_ADD_DRIVER_FILES MESA_GL_VERSION_OVERRIDE MESA_GLSL_VERSION_OVERRIDE
export LIBGL_DRIVERS_PATH="$SPOOL_TEST_DRI_DIR"
export LD_LIBRARY_PATH="$SPOOL_TEST_DRIVER_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export VK_DRIVER_FILES="$SPOOL_TEST_VULKAN_ICD"
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
export QT_OPENGL=desktop
export __EGL_VENDOR_LIBRARY_FILENAMES="$SPOOL_TEST_EGL_VENDOR"
export __GLX_VENDOR_LIBRARY_NAME=mesa
case "${SPOOL_TEST_DISPLAY_BACKEND:-weston}" in
  weston)
    export WAYLAND_DISPLAY=spool-tests QT_QPA_PLATFORM=wayland
    weston --backend=headless --renderer=gl --width=1920 --height=1080 \
      --socket="$WAYLAND_DISPLAY" --no-config --idle-time=0 \
      --log="$work/compositor.log" >"$work/display.log" 2>&1 &
    display_pid=$!
    for _ in {1..100}; do
      [[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]] && break
      kill -0 "$display_pid" 2>/dev/null || break
      sleep 0.1
    done
    if ! kill -0 "$display_pid" 2>/dev/null || [[ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]]; then
      cat "$work/display.log" "$work/compositor.log" >&2 || true
      echo 'error: isolated Weston GL compositor did not become ready' >&2
      exit 1
    fi
    ;;
  xvfb)
    # -displayfd allocates an unused display instead of borrowing :0 or :99.
    Xvfb -displayfd 3 -screen 0 1920x1080x24 -nolisten tcp +extension GLX \
      3>"$work/display-number" >"$work/display.log" 2>&1 &
    display_pid=$!
    for _ in {1..100}; do
      [[ -s "$work/display-number" ]] && break
      kill -0 "$display_pid" 2>/dev/null || break
      sleep 0.1
    done
    if ! kill -0 "$display_pid" 2>/dev/null || [[ ! -s "$work/display-number" ]]; then
      cat "$work/display.log" >&2
      echo 'error: isolated Xvfb GLX display did not become ready' >&2
      exit 1
    fi
    read -r number <"$work/display-number"
    [[ "$number" =~ ^[0-9]+$ ]] || { echo 'error: invalid Xvfb display number' >&2; exit 1; }
    export DISPLAY=":$number" QT_QPA_PLATFORM=xcb
    ;;
  *) echo 'error: SPOOL_TEST_DISPLAY_BACKEND must be weston or xvfb' >&2; exit 2 ;;
esac
# The selectors choose OpenGL/Vulkan explicitly and validate actual video pixels.
# A compositor starting is not itself proof that the application rendered.
"$@"
