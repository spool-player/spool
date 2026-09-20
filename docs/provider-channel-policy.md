# Portable provider channel policy and review notes

Status: implementation/review preparation, not a store approval or a claim that the package manager exists. Checked primary policy text on 21 September 2026.

## Current implementation

The app still uses its bundled native Jellyfin client. The experimental worker and source-package tooling do not expose an application installer or remote activation path. No remote switch can enable downloaded provider code. The standalone source ZIP validator does not authenticate its publisher, and the runtime is not a hostile-code sandbox.

## Required delivery policy

| Channel | Bundled source packages | Downloaded source activation |
| --- | --- | --- |
| Windows/Linux/direct macOS | Same pinned provider sources | Only after authenticated catalogue, verified archive installation and restart recovery are implemented |
| webOS Developer Mode | Same pinned provider sources | Same trust gate, plus actual unrooted app-data loading/import/interpreter validation |
| Android outside Play | Same pinned provider sources | Same trust gate and app-private storage; no native provider payloads |
| Android/TV Google Play | Bundle official packages regardless | Seek review of the interpreted JS/QML host surface; never an APK self-updater or native library installer |
| iOS/tvOS/Mac App Store | Multiple bundled providers from the same source implementations | Keep disabled pending review/permission for the actual hosted software and native API exposure |

Package `channels` declarations express eligibility, not authority. Only the host's compiled distribution policy and verified publisher/catalogue metadata may permit activation. A provider must not grant itself new access, enable an installer, or change the channel.

## Google Play reviewer explanation

Proposed feature: Spool is a native media client for user-authorised media servers. Portable providers implement endpoint/authentication protocols and optional Qt Quick screens. They are not remote media data interpreted as program code. The native application continues to own transport/TLS, media models, playback/decoding/rendering and platform integration.

The experimental implementation uses a separate worker QJSEngine for protocol logic. Provider operations receive an origin-scoped HTTP facade, not an Android Context, general shell, arbitrary filesystem or unrestricted player commands. Redirects and implicit cookie propagation are disabled. Source removal and watchdog interruption settle pending operations. GUI-side QML is a separate execution surface and must be reviewed independently; worker origin checks alone do not constrain every QML network/resource loader.

The intended delivery is reviewed source-only ZIPs with manifests and explicit access requirements. Archive tooling rejects native executable payloads, plugin declarations by unsupported file type, unsafe paths, symlinks and size-limit violations. These static checks do not replace content review or a secure catalogue. The final implementation must authenticate artifacts and metadata, enforce freshness/revocation, preserve accounts on incompatible updates, and provide disable/recovery controls before downloaded activation is submitted.

There is no intended APK, DEX, JAR or native `.so` download path in the Play channel. Host updates must use Google Play. The app must not use provider delivery to bypass permission, payment, intellectual-property, privacy, network-abuse or content policies.

### Evidence required before submitting the feature

- An actual Android/TV build containing the declared Qt/QML import set and showing both bundled and verified source-loaded operation.
- Reviewer-accessible demo server and synthetic/public-domain media, not personal credentials.
- Recording of install/account setup, access consent, version inspection, update/restart, incompatible-package rejection and disable/recovery.
- Exact host-facade and native-type exposure inventory, including QML imports, dynamic loading, external navigation and network-capable controls.
- Signed metadata/rotation/revocation design and tests; package validation alone is insufficient.
- Privacy/Data Safety declarations that match credentials, configured server URLs, diagnostics and remote-provider access.
- Content-access policy and moderation/reporting mechanisms appropriate to the actual offered services.
- Confirmation that downloaded packages cannot enable a host updater, invoke arbitrary processes, or load native plugins.

These materials are an engineering/review checklist, not completed submissions. The current implementation is not ready for the downloaded-code submission.

## Policy basis

Google's [Device and Network Abuse full policy](https://support.google.com/googleplay/android-developer/answer/16559646?hl=en) prohibits self-updates outside Play and downloading executable code such as DEX/JAR/native libraries, with an exception for code running in a VM/interpreter providing indirect Android API access. It separately requires runtime-loaded interpreted code not to allow potential Play-policy violations. The exception does not automatically approve Qt JS/QML provider delivery.

Apple's [App Review Guidelines](https://developer.apple.com/app-store/review/guidelines/) section 4.7 permits certain non-embedded software but requires prior permission to expose native platform APIs/technologies (4.7.2), explicit consent for data/permission sharing (4.7.3), a software index with universal links (4.7.4), and age/content safeguards. Sections 2.5.2 and the Mac App Store-specific 2.4.5 rules also require review of the actual distribution design. Until that review is resolved, use bundled-provider updates through app releases; do not promise approval or remotely unlock downloading.
