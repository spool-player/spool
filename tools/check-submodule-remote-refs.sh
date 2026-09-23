#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:---head}"
ZERO_SHA="0000000000000000000000000000000000000000"

if [[ "${SPOOL_SKIP_SUBMODULE_REMOTE_CHECK:-0}" == "1" ]]; then
  echo "Skipping submodule remote check because SPOOL_SKIP_SUBMODULE_REMOTE_CHECK=1" >&2
  exit 0
fi

case "$MODE" in
  --head|--pre-push) ;;
  *)
    echo "usage: $0 [--head|--pre-push]" >&2
    exit 2
    ;;
esac

module_rows() {
  git -C "$ROOT" config -f "$ROOT/.gitmodules" --get-regexp '^submodule\..*\.path$' |
    while read -r key path; do
      local name
      name="${key#submodule.}"
      name="${name%.path}"
      printf '%s\t%s\n' "$name" "$path"
    done
}

commits_from_pre_push() {
  local local_ref local_sha remote_ref remote_sha
  while read -r local_ref local_sha remote_ref remote_sha; do
    [[ -n "${local_sha:-}" ]] || continue
    [[ "$local_sha" != "$ZERO_SHA" ]] || continue
    if [[ "${remote_sha:-}" == "$ZERO_SHA" || -z "${remote_sha:-}" ]]; then
      printf '%s\n' "$local_sha"
    else
      git -C "$ROOT" rev-list "$remote_sha..$local_sha"
    fi
  done
}

collect_submodule_refs() {
  local commit name path entry mode type sha rest
  while read -r commit; do
    [[ -n "$commit" ]] || continue
    while IFS=$'\t' read -r name path; do
      [[ -n "$path" ]] || continue
      entry="$(git -C "$ROOT" ls-tree "$commit" -- "$path" || true)"
      [[ -n "$entry" ]] || continue
      read -r mode type sha rest <<<"$entry"
      [[ "$mode" == "160000" && "$type" == "commit" && -n "$sha" ]] || continue
      printf '%s\t%s\t%s\t%s\n' "$name" "$path" "$sha" "$commit"
    done < <(module_rows)
  done | sort -u
}

check_ref() {
  local name="$1" path="$2" sha="$3" parent_commit="$4"
  local url branch submodule_dir
  url="$(git -C "$ROOT" config -f "$ROOT/.gitmodules" --get "submodule.$name.url" || true)"
  branch="$(git -C "$ROOT" config -f "$ROOT/.gitmodules" --get "submodule.$name.branch" || true)"
  [[ -n "$url" ]] || url="$(git -C "$ROOT/$path" remote get-url origin 2>/dev/null || true)"
  [[ -n "$branch" ]] || branch="HEAD"
  submodule_dir="$ROOT/$path"

  if [[ ! -d "$submodule_dir" ]]; then
    echo "error: submodule checkout missing: $path" >&2
    echo "       Run: git submodule update --init -- $path" >&2
    return 1
  fi
  if ! git -C "$submodule_dir" rev-parse --git-dir >/dev/null 2>&1; then
    echo "error: $path is not an initialized git checkout" >&2
    echo "       Run: git submodule update --init -- $path" >&2
    return 1
  fi
  if ! git -C "$submodule_dir" cat-file -e "$sha^{commit}" 2>/dev/null; then
    echo "error: parent commit $parent_commit points $path at $sha, but that commit is not present locally" >&2
    echo "       Run: git -C $path fetch --all --tags, then retry" >&2
    return 1
  fi
  if [[ -z "$url" ]]; then
    echo "error: no remote URL configured for submodule $path" >&2
    return 1
  fi

  echo "Checking $path $sha is reachable from $url $branch" >&2
  if ! git -C "$submodule_dir" fetch --quiet "$url" "$branch"; then
    echo "error: failed to fetch $branch from $url for submodule $path" >&2
    return 1
  fi
  if ! git -C "$submodule_dir" merge-base --is-ancestor "$sha" FETCH_HEAD; then
    cat >&2 <<EOF
error: parent commit $parent_commit points $path at $sha,
       but $url branch $branch does not contain that commit.

Push the submodule commit first, then retry the parent repository push:
  git -C $path push origin $branch
EOF
    return 1
  fi
}

refs_file="$(mktemp)"
trap 'rm -f "$refs_file"' EXIT

if [[ "$MODE" == "--pre-push" ]]; then
  commits_from_pre_push | collect_submodule_refs >"$refs_file"
else
  git -C "$ROOT" rev-parse HEAD | collect_submodule_refs >"$refs_file"
fi

if [[ ! -s "$refs_file" ]]; then
  exit 0
fi

failed=0
while IFS=$'\t' read -r name path sha parent_commit; do
  check_ref "$name" "$path" "$sha" "$parent_commit" || failed=1
done <"$refs_file"

exit "$failed"
