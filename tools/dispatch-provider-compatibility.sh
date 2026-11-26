#!/usr/bin/env bash
set -euo pipefail

state_ref=refs/heads/ci/provider-compatibility-state
workflow=provider-compatibility.yml
candidates="$1"
repo="$2"
base="$3"
server="$4"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

if [[ ! -s "$candidates" ]]; then
  echo 'No new stable provider images are ready.'
  exit 0
fi

# Use an orphan automation branch, independent of release and topic branches.
git init --quiet "$scratch"
git -C "$scratch" remote add origin "$server/$repo.git"
if git -C "$scratch" ls-remote --exit-code origin "$state_ref" >/dev/null; then
  git -C "$scratch" fetch --quiet --depth=1 origin "$state_ref"
  git -C "$scratch" checkout --quiet --detach FETCH_HEAD
else
  status=$?
  if [[ "$status" != 2 ]]; then exit "$status"; fi
fi

while IFS=$'\t' read -r provider version digest extra; do
  case "$provider" in
    jellyfin) name=Jellyfin; [[ "$version" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] ;;
    emby) name=Emby; [[ "$version" =~ ^[0-9]+(\.[0-9]+){3}$ ]] ;;
    plex) name=Plex; [[ "$version" =~ ^[0-9]+(\.[0-9]+){3}-[0-9a-f]{9}$ ]] ;;
    *) echo 'Unsupported upstream provider' >&2; exit 1 ;;
  esac
  [[ "$digest" =~ ^sha256:[0-9a-f]{64}$ && -z "$extra" ]]
  record="$provider/$version.json"
  if [[ -f "$scratch/$record" ]]; then
    echo "$name $version already has a compatibility dispatch receipt."
    continue
  fi

  # Recover after a cancellation between the API accepting the dispatch and
  # the state branch push. Failed runs count as dispatched and can be re-run.
  title="$name $version $digest"
  run_id="$(gh api --method GET --paginate "repos/$repo/actions/workflows/$workflow/runs" \
    -f event=workflow_dispatch -f branch="$base" -f per_page=100 \
    --jq '.workflow_runs[] | [.display_title, .id] | @tsv' \
    | awk -F '\t' -v title="$title" '$1 == title && !found++ {print $2}')"
  if [[ -z "$run_id" ]]; then
    run_id="$(jq -n --arg ref "$base" --arg provider "$provider" --arg version "$version" --arg digest "$digest" \
      '{ref: $ref, return_run_details: true, inputs: {provider: $provider, version: $version, image_digest: $digest}}' \
      | gh api --method POST -H 'X-GitHub-Api-Version: 2026-03-10' \
        "repos/$repo/actions/workflows/$workflow/dispatches" --input - --jq '.workflow_run_id')"
  fi
  [[ "$run_id" =~ ^[0-9]+$ ]]

  mkdir -p "$scratch/$provider"
  jq -n --arg version "$version" --arg digest "$digest" --argjson run "$run_id" \
    '{version: $version, imageDigest: $digest, workflowRun: $run}' >"$scratch/$record"
  git -C "$scratch" add -- "$record"
  git -C "$scratch" -c user.name=github-actions -c user.email=41898282+github-actions[bot]@users.noreply.github.com \
    commit --quiet -m "Record $name $version compatibility run"
  git -C "$scratch" push --quiet origin "HEAD:$state_ref"
  printf -- '- %s %s [Actions run](%s/%s/actions/runs/%s)\n' "$name" "$version" "$server" "$repo" "$run_id"
done <"$candidates"
