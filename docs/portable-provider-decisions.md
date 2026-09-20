# Portable provider implementation decisions

## Baseline

Inspected `refactor/provider-seam` at `e0de68c18c3740bc22421b99848fa9f7b2f0e788`.
`nix run .#tests` built the native release app and passed 77/77 tests before the implementation changes. This is desktop correctness evidence, not TV performance evidence.

The current application has a single active `Provider`, source-global artwork and playback credentials, native Jellyfin authentication/API logic, and hardcoded Jellyfin route components. These remain migration work until explicitly marked completed below; the existence of a script runtime does not complete their replacement.

## Jellyfin parity inventory

The inspected `JellyfinApiFacade` surface requires all of the following before removing the native client:

- Authentication by name and QuickConnect; current account identity, configuration and policy; cultures; discovery and manual server entry; stored profiles and credentials.
- Libraries, query options and filters, bounded browse pages, item details, bulk ID lookups, seasons, episodes, resume, next-up, latest, search, suggestions, similar titles, and person credits.
- Playlists: create, append/insert, remove, move and rename. Collections: create, append and remove. Item rename and deletion.
- Favourite, played and playback-position state.
- Media-source selection, subtitle and audio selection, direct/remux/transcoded negotiation, device profiles, quality limits, bandwidth policy, segments and trickplay.
- Start/progress/stop reports, session capabilities, remote-session discovery and commands.
- SyncPlay groups, server clock and ping, buffering, scheduled controls, queue replacement/editing and WebSocket events.
- Provider login/profile UI, management, remote-control and SyncPlay UI, plus account settings integration.

Native transport, playback, rendering, bandwidth measurement and generic timing policies should remain native. Endpoint construction, backend parsing and backend session protocol belong in the independently packaged client.

## Decisions

### QML warming

Retain `QQmlComponent` objects rather than instantiate every provider screen. Compilation uses `QQmlComponent::Asynchronous`, begins after the first frame plus a delay, and processes one component at a time. This avoids running hidden authentication flows, completion handlers or timers. Existing memory-budgeted route-instance caching serves a separate purpose and remains in place. Memory pressure abandons warming for the process lifetime and releases retained components. Retention is not a guarantee that every frame or object creation will be below 100 ms.

### Experimental worker profile

The draft worker profile uses module exports, explicit source factories and Promise-based operations. It does not assume Node/browser globals or async-function syntax. Host-authorised HTTP origins and persistent source IDs are supplied by native callers. Backend JS and its native transport live on one worker thread per runtime; native owned results cross the thread boundary. An interrupted module must be discarded, not silently resumed.

This in-process runtime is for trusted/reviewed code. A QJSEngine is not a sandbox, and a host facade alone cannot enforce downloaded-QML security. Independent downloaded-package activation must not be enabled until package verification and channel policy are implemented.

## Unexercised external gates

No webOS device was built, deployed or tested. Minimum supported device/firmware performance, interpreter-only on-device timings, Android/TV and Apple/Google store approvals remain unverified. Desktop test timings must not be represented as those results.
