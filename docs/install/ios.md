# Installing Spool on iPhone and iPad

Spool is not on the App Store. Each release publishes an unsigned iOS app
(`Spool-<version>-iOS-arm64.ipa`) that SideStore or AltStore installs by signing
it with your own Apple ID. A free Apple ID is enough.

Spool needs iOS 17 or later.

## With SideStore or AltStore

1. Install [SideStore](https://sidestore.io) or [AltStore](https://altstore.io)
   on the device, following their own setup guide.
2. In SideStore or AltStore, open **Sources**, choose **Add**, and enter:

   ```
   https://spool-player.github.io/spool/altstore.json
   ```

3. Open the Spool source and install Spool.

Updates appear in the same place whenever a new version is released.

## Installing a downloaded IPA by hand

Download `Spool-<version>-iOS-arm64.ipa` from the
[releases page](https://github.com/spool-player/spool/releases), open it with
SideStore or AltStore (**My Apps → +**), and sign in with your Apple ID when asked.
This route does not update itself; the source above does.

## What a free Apple ID means

- Apps signed with a free Apple ID stop opening after **7 days** unless they are
  refreshed. SideStore and AltStore refresh them in the background; open them
  now and then if Spool asks to be refreshed.
- A free Apple ID can have **3** sideloaded apps installed at once, SideStore
  or AltStore included.
- Spool asks for no special entitlements, so nothing is lost by signing it this
  way. Your accounts and settings stay on the device across refreshes and updates.

## Building and signing it yourself

With a Mac, Xcode and a paid developer account you can build a signed copy:

```
APPLE_DEVELOPMENT_TEAM=<team id> APPLE_ARCHIVE=1 \
APPLE_EXPORT_OPTIONS=/path/to/ExportOptions.plist bash tools/build-ios.sh
```

`SPOOL_APPLE_APP_STORE=OFF` builds the same open flavour the release IPA uses
(every bundled provider, and providers from links); the default is the App Store
flavour, which bundles only App Store-approved providers.
