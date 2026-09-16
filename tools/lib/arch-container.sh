#!/usr/bin/env bash
# Shared helpers for the scripts that need makepkg.
#
# Source this file ("source tools/lib/arch-container.sh"); do not execute it.
# It defines functions only and changes no global state.

# Re-run the calling script inside an Arch container, and return without doing
# anything once already in one. makepkg is the only thing that writes pacman
# metadata worth trusting and it runs on Arch alone, so the container is the
# whole of that dependency.
#
# usage: arch_container_reexec REPO_ROOT REPO_RELATIVE_SCRIPT [ARG...]
arch_container_reexec() {
  local root="$1"
  local script="$2"
  shift 2
  local image="${SPOOL_ARCH_IMAGE:-archlinux:base-devel}"
  local runtime="${SPOOL_CONTAINER_RUNTIME:-}"
  local candidate

  if [[ "${SPOOL_ARCH_IN_CONTAINER:-0}" == 1 ]]; then
    return 0
  fi
  if [[ -z "$runtime" ]]; then
    for candidate in docker podman; do
      command -v "$candidate" >/dev/null 2>&1 && { runtime="$candidate"; break; }
    done
  fi
  [[ -n "$runtime" ]] || {
    echo "error: this step needs docker or podman to run $image" >&2
    return 1
  }
  exec "$runtime" run --rm \
    -e SPOOL_ARCH_IN_CONTAINER=1 \
    -e "SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-0}" \
    -v "$root:/spool" -w /spool "$image" \
    bash "$script" "$@"
}

# makepkg refuses to run as root, and the container is root. Everything that
# calls makepkg goes through these so the unprivileged user exists once.
arch_container_ensure_builder() {
  id -u builder >/dev/null 2>&1 || useradd -m builder
}

arch_container_runuser() {
  arch_container_ensure_builder
  runuser -u builder -- "$@"
}

# Give a file the ownership of the mounted checkout rather than the container's
# root, so a host build does not leave unwritable files behind.
arch_container_restore_owner() {
  local reference="$1"
  shift
  chown "$(stat -c '%u:%g' "$reference")" "$@"
}
