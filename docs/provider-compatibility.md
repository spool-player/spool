# Provider compatibility

The bundled provider archives are pinned in `providers/lock.json`. Provider protocol tests exercise the same archives through the app's registry and `SourceHub`. The private GUI journey exercises the actual app through its QML UI and observes playback and provider requests. These are separate checks with different coverage.

| Provider | Current evidence in this repository | Authentic upstream environment |
| --- | --- | --- |
| Local | `local-provider` exercises folder setup, scans, browsing, search, item state, and playback resolution against checked-in media. | A temporary local directory is the source. The player requires no server account. |
| Jellyfin | `app-journey` uses a stateful loopback server and the shipped provider. `live-jellyfin` signs in to real Jellyfin, checks rejected credentials, browses, mutates favorites and resume state, resolves and reads media, reports playback, and exchanges remote and group events. | The upstream server starts with fresh directories, a synthetic movie, and an isolated test identity. The Linux artifact workflow uses Jellyfin 12.4 from an immutable Nixpkgs revision and checks the server-reported version. |
| Emby | `emby-provider-compatibility` checks discovery, credential rejection, sign-in, browsing, search, favorites, authenticated video decoding, and playback report calls. The fake HTTP server also counts media reads and reporting requests. | The Linux artifact workflow provisions a fresh server-local user and synthetic movie in the official [Emby 4.10.2.0 container](https://docs.emby.media/docker-server.html), pinned by image digest. Emby Connect and paid capabilities are outside this fixture. |
| Plex | `plex-provider-compatibility` probes the server identity, loads a PMS-only profile, browses and searches the local library, rejects a missing item, updates favorites, decodes video, and reports playback. The fake server additionally counts media reads and timeline requests. | An [official Plex Media Server image](https://github.com/plexinc/pms-docker) is pinned to `1.43.5.11079-b3b13e506` by digest. The [vendor's scanner](https://support.plex.tv/articles/201242707-plex-media-scanner-via-command-line/) creates the synthetic library on an unclaimed server. Cloud sign-in, PINs, resource discovery, and Plex Home remain outside this local fixture. |
| Stremio | `stremio-addon-provider` covers manifest rejection, setup, catalogue, search, capability rejection, picker cancellation, HTTP decoding, and original download negotiation. The shipped 0.1.4 package fails activation because Qt does not implement its `Object.fromEntries` call. CI records the failure without blocking artifact assembly. | The [official add-on SDK](https://github.com/Stremio/stremio-addon-sdk) is locked to 1.1.4 with integrity-pinned dependencies. A separate minimal HTTP fixture runs the same assertions. Both serve synthetic media. Coverage is limited to HTTP streams and leaves torrent-engine behavior untested. |
| Open Movies | `open-movies-provider` exercises credential-free setup, browsing, search, retry after a missing directory, media-file filtering, HTTPS playback, and original download negotiation. | The [Blender download mirror](https://download.blender.org/) exposes static directory listings. The fake HTTP fixture and the pinned Nix nginx autoindex exercise that protocol with synthetic files. A private CONNECT proxy accepts only the mirror hostname and sends traffic to the private fixture using a trust anchor scoped to the child process. |

Run the pinned Jellyfin check after `nix run .#build` on Linux.

```sh
server="$(nix build --no-link --print-out-paths \
  'github:NixOS/nixpkgs/0bb8272318b4ca41bf0edc6224df3e3e3046b70d#jellyfin')"
nix develop .#native -c env PYTHONDONTWRITEBYTECODE=1 \
  python tests/e2e/jellyfin_provider_compatibility.py \
  --jellyfin-bin "$server/bin/jellyfin" --version 12.4 \
  --spool-tests build/linux-release/app/spool-tests --report /tmp/jellyfin-compatibility.json
```

The receipt contains versions, phase, and exit status. Test account credentials, authenticated URLs, and process logs remain private. The Linux private display can exercise the shared QML, keyboard navigation, and the actual Linux video pipeline. Apple and Android simulator checks require their dedicated runners. Physical remote gestures, platform media controls, and device video output require device validation.

Run both Stremio fixtures against the built Linux host.

```sh
npm --prefix tests/e2e/fixtures/stremio-addon ci --ignore-scripts --no-audit --no-fund
nix develop .#native -c python tests/e2e/stremio_provider_compatibility.py \
  --spool-tests build/linux-release/app/spool-tests --report /tmp/stremio-compatibility.json
```

## Upstream releases

The **Provider compatibility** workflow discovers upstream releases daily on the default branch. A manual run with the version and digest empty performs the same scan. The scan supports Jellyfin, Emby, and Plex and waits for an official Linux x86-64 image before dispatch. Each candidate is tested by image digest using the same app contract and synthetic media as the pinned checks. Each container publishes only a loopback port and is removed when the test finishes.

The workflow stores dispatch receipts on `ci/provider-compatibility-state`. Each receipt records a version, image digest, and Actions run ID. A repeated scan skips recorded versions. If cancellation happens after dispatch and before the receipt is pushed, the next scan checks the workflow's run history for that digest. The dispatch job can write Actions runs and the receipt branch. The compatibility job has read-only repository access. Re-run a failed candidate on its existing Actions run or dispatch the version and digest explicitly.
