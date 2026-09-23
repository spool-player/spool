#!/usr/bin/env bash
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

mapfile -d '' cpp_files < <(
  git diff --cached --name-only --diff-filter=ACMR -z -- \
    '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx'
)
mapfile -d '' qml_files < <(
  git diff --cached --name-only --diff-filter=ACMR -z -- '*.qml'
)

if (( ${#cpp_files[@]} == 0 && ${#qml_files[@]} == 0 )); then
  exit 0
fi

for file in "${cpp_files[@]}" "${qml_files[@]}"; do
  if ! git diff --quiet -- "$file"; then
    echo "error: cannot format partially staged file: $file" >&2
    echo "Stage or stash its remaining changes before committing." >&2
    exit 1
  fi
done

clang_format="${CLANG_FORMAT:-$(command -v clang-format || true)}"
qml_format="${QMLFORMAT:-$(command -v qmlformat || true)}"
if { (( ${#cpp_files[@]} > 0 )) && [[ -z "$clang_format" ]]; } \
    || { (( ${#qml_files[@]} > 0 )) && [[ -z "$qml_format" ]]; }; then
  if [[ "${SPOOL_FORMAT_HOOK_IN_NIX:-0}" != "1" ]] && command -v nix >/dev/null 2>&1; then
    exec nix develop .#native -c env SPOOL_FORMAT_HOOK_IN_NIX=1 "$0"
  fi
  [[ ${#cpp_files[@]} == 0 || -n "$clang_format" ]] \
    || { echo "error: clang-format is required to commit C++ changes" >&2; exit 1; }
  [[ ${#qml_files[@]} == 0 || -n "$qml_format" ]] \
    || { echo "error: qmlformat is required to commit QML changes" >&2; exit 1; }
fi

if (( ${#cpp_files[@]} > 0 )); then
  "$clang_format" -i --style=file -- "${cpp_files[@]}"
  git add -- "${cpp_files[@]}"
fi
if (( ${#qml_files[@]} > 0 )); then
  "$qml_format" -i -- "${qml_files[@]}"
  git add -- "${qml_files[@]}"
fi
