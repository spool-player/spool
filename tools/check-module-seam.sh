#!/usr/bin/env bash
# Fails when core source reaches into the Jellyfin provider.
#
# Core is everything CMakeLists.txt lists in SPOOL_PLATFORM_SOURCES,
# SPOOL_PLAYER_SOURCES and SPOOL_SHELL_SOURCES, plus every file under the
# directories that are core by construction. A core file may include only
# core files, Qt and system headers: any include that resolves into src/api/
# or src/discovery/ is a seam violation. With --strict, a file that only
# SPOOL_JELLYFIN_SOURCES lists counts as provider too; that mode still
# reports the composition-root leaks (AppController reached from the
# platform and diagnostics layers) that the provider registry work removes,
# so CI runs the default. The check is on direct includes; because every
# core file is checked, a transitive path through core is caught at the file
# that opens it.
set -euo pipefail

strict=0
for arg in "$@"; do
  case "$arg" in
    --strict) strict=1 ;;
    *) echo "usage: $0 [--strict]" >&2; exit 2 ;;
  esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

core_dirs=(src/platform src/player src/media src/provider src/common src/cache src/diagnostics)
provider_dirs=(src/api src/discovery src/providers)

group_files() {
  sed -n "/^set($1/,/^)/p" CMakeLists.txt | grep -E '^\s+src/' | sed -E 's/^\s+//'
}

declare -A core=()
for group in SPOOL_PLATFORM_SOURCES SPOOL_PLAYER_SOURCES SPOOL_SHELL_SOURCES; do
  while IFS= read -r file; do
    [[ -n "$file" ]] && core["$file"]=1
  done < <(group_files "$group")
done
for dir in "${core_dirs[@]}"; do
  while IFS= read -r file; do
    core["$file"]=1
  done < <(find "$dir" -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.mm' \) | sort)
done

declare -A provider=()
while IFS= read -r file; do
  [[ -n "$file" ]] && provider["$file"]=1
done < <(group_files SPOOL_JELLYFIN_SOURCES)

is_provider_path() {
  local path="$1"
  for dir in "${provider_dirs[@]}"; do
    [[ "$path" == "$dir/"* ]] && return 0
  done
  (( strict )) && [[ -n "${provider[$path]:-}" ]] && [[ -z "${core[$path]:-}" ]]
}

status=0
for file in "${!core[@]}"; do
  [[ -f "$file" ]] || continue
  dir="$(dirname "$file")"
  while IFS= read -r include; do
    # Quoted includes resolve against the including file's directory first
    # and the source root second; that is how the build resolves them too.
    resolved=""
    for candidate in "$dir/$include" "src/$include" "$include"; do
      if [[ -f "$candidate" ]]; then
        resolved="$(realpath --relative-to="$ROOT" "$candidate")"
        break
      fi
    done
    [[ -n "$resolved" ]] || continue
    if is_provider_path "$resolved"; then
      echo "seam: $file includes provider file $resolved" >&2
      status=1
    fi
  done < <(grep -oE '^\s*#\s*include\s+"[^"]+"' "$file" | sed -E 's/.*"([^"]+)"/\1/')
done

if (( status != 0 )); then
  echo "error: core sources may not include provider files" >&2
  exit 1
fi
echo "module seam: core sources are free of provider includes"
