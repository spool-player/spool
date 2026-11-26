#!/usr/bin/env bash
set -euo pipefail

project="$(cd "$(dirname "$0")/../.." && pwd)"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
mkdir -p "$scratch/remote/test" "$scratch/bin"
git init --quiet --bare "$scratch/remote/test/repo.git"

# Keep Git's actual durable state behavior and simulate only the GitHub API.
export SPOOL_TEST_ACTIONS_RUNS="$scratch/runs.tsv"
export SPOOL_TEST_ACTIONS_POSTS="$scratch/posts"
: >"$SPOOL_TEST_ACTIONS_RUNS"
: >"$SPOOL_TEST_ACTIONS_POSTS"
cat >"$scratch/bin/gh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
case "$3" in
  GET) cat "$SPOOL_TEST_ACTIONS_RUNS" ;;
  POST)
    jq -e '.ref == "master" and .return_run_details and .inputs.provider == "jellyfin" and .inputs.version == "12.5"' >/dev/null
    echo 123 >>"$SPOOL_TEST_ACTIONS_POSTS"
    echo 123
    ;;
  *) exit 2 ;;
esac
EOF
chmod +x "$scratch/bin/gh"
export PATH="$scratch/bin:$PATH"
state_ref=refs/heads/ci/provider-compatibility-state
digest="sha256:$(printf '%064d' 1)"
candidates="$scratch/candidates.tsv"
printf 'jellyfin\t12.5\t%s\n' "$digest" >"$candidates"

dispatch() {
  bash "$project/tools/dispatch-provider-compatibility.sh" "$candidates" test/repo master "file://$scratch/remote"
}

dispatch
dispatch
test "$(wc -l <"$SPOOL_TEST_ACTIONS_POSTS")" -eq 1
git -C "$scratch/remote/test/repo.git" show "$state_ref:jellyfin/12.5.json" \
  | jq -e --arg digest "$digest" '.workflowRun == 123 and .imageDigest == $digest' >/dev/null

# A completed dispatch with no receipt is recovered from the exact run title.
printf 'emby\t4.10.3.0\t%s\n' "$digest" >>"$candidates"
printf 'Emby 4.10.3.0 %s\t456\n' "$digest" >"$SPOOL_TEST_ACTIONS_RUNS"
dispatch
test "$(wc -l <"$SPOOL_TEST_ACTIONS_POSTS")" -eq 1
git -C "$scratch/remote/test/repo.git" show "$state_ref:emby/4.10.3.0.json" \
  | jq -e '.workflowRun == 456' >/dev/null
