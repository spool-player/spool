# Contributing

Spool for Jellyfin welcomes focused bug reports and feature proposals. Before submitting code, open or find an issue for consequential behavior changes so the user-facing contract is clear.

## Privacy

Use synthetic accounts and media. Never post access tokens, passwords, Quick Connect codes or secrets, authorization headers, cookies, raw logs, server URLs or addresses, account names, library names, media titles, or stable Jellyfin item/profile IDs. Use the in-app diagnostics export after reviewing its preview.

## Development

The supported native environment is Nix:

```sh
nix run .#build
nix develop .#native -c ctest --test-dir build/linux-release/app --output-on-failure
```

Entering any Nix development shell configures the repository's pre-commit and
pre-push hooks automatically. To enable them without entering a development
shell, run:

```sh
./tools/install-git-hooks.sh
```

Keep changes small and cohesive. Add tests for new observable contracts. Run the relevant targeted tests, strict QML lint, and QML import scan. Do not include generated build output.

## Pull requests

Describe the problem, the chosen behavior, affected platforms, privacy/security implications, and exact verification performed. Preserve TV D-pad behavior when changing QML navigation. Platform packaging changes must retain pinned inputs, checksums, license material, and package smoke checks.

### Reviewing CI artifacts

Open the PR's **build artifacts** check, follow **Details** to the workflow run,
and download a `spool-for-jellyfin-*` package from **Artifacts** (GitHub sign-in
required). PRs use the same build, package, audit, and launch-test jobs as
releases: Linux AppImage/portable tarball/Arch package, macOS DMGs for Apple
Silicon and Intel, webOS IPK, Android per-ABI/universal APKs, and Windows
portable/installer executables. Extract the downloaded artifact ZIP first;
`internal-*` artifacts and macOS `.dmg.app.tar.gz` files are build/analysis
inputs, not installers. Review packages are retained for seven days.

These are untrusted review builds, not releases: only run code you have
reviewed, using synthetic accounts/media. macOS packages are not Developer ID
signed or notarized; Android APKs use a debug key and cannot replace an
installed release (use a clean test device/emulator). PRs never publish
releases or update metadata.

Branch pushes build independently until the branch has an open PR in the
same repository; then the PR run owns the expensive jobs. The read-only
lookup matches the exact head repository/branch, not its SHA, so a brief PR
metadata synchronization delay does not duplicate builds. Lookup failures
warn and build rather than silently losing coverage. Master, release tags,
and manual runs are not suppressed. Dependency review remains a separate PR
check.

For branch protection, require the platform checks on the PR's merge commit
and the separate **Dependency review** check, not **Select artifact build**
alone: that selection check only decides whether another run owns the build.
Existing platform check names are unchanged; a duplicate push run will show
them as skipped, so reviewers must use the PR run for results and downloads.

### CI cache and credential policy

CI restores public caches for every PR. Only push and manually dispatched
builds publish to Cachix or save/prune GitHub caches; publishing tokens are
limited to the upload steps, and signing credentials to release builds.
Qt cache keys use the pinned derivation, not the branch or application source.
Cachix shares identical native Qt builds across branches; GitHub caches are
branch-scoped and fall back to the default branch, so a PR cannot warm master's
GitHub cache. macOS input overrides retain the remaining lock-file pins.
Different Qt profiles, architectures, or dependency pins still require distinct
builds. Fork PRs receive no repository secrets. GitHub may permit a modified PR
workflow to create caches in its own merge-ref scope, but those cannot replace
master's cache or publish to Cachix.

By participating, you agree to follow `CODE_OF_CONDUCT.md`.
