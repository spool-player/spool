# Local repeat controls

Open **Playback settings → Repeat** while local audio or video is playing:

| Mode | At the end of an item | Explicit Next / Previous |
| --- | --- | --- |
| Off | Advance normally; episodes can continue into fetched successors. | Move within the queue. |
| Repeat queue | Advance through the current playback order, wrapping at the end. | Wrap at either end. |
| Repeat current item | Restart the current item from the beginning. | Move within the queue, bypassing repeat-one. |

Repeat queue follows the queue's playback order, including shuffle and manual
reordering. A one-item queue can repeat. Clearing the queue resets repeat to Off;
replacing it keeps the chosen mode. The setting lasts for this app session and is
reset on logout.

Remote repeat commands and the local menu change the same state. Playback
start/progress reports publish that state along with the current queue and
playback order, so a remote controller can reflect local changes.

A playback group owns its queue policy. Joining a group clears local repeat and
disables the local Repeat menu; group playback reports omit the local repeat and
shuffle fields. Leaving the group starts with local repeat Off. This control does
not claim to change the group's repeat mode. Native playlist playback keeps its
own player-side traversal and ignores queue repeat.

The queue model owns the single repeat state and the wrap decisions.

## Sleep timer

Open **Playback settings → Sleep timer** during local audio or video playback.
Choose **After current item** or **15, 30, 45, 60, 90, or 120 minutes**. Choose
**Off** to cancel. The playback settings row shows the remaining duration.

- A duration counts elapsed time from when it is selected. Pausing, buffering,
  changing items, and restarting a stream for quality or codec changes do not
  restart the countdown. Selecting another duration starts a new countdown.
- After current item stops once when that item completes, before repeat or
  automatic queue/episode advance. Quality and codec restarts of the same item
  keep it armed. Starting a different item cancels it.
- Only one request can be active. Selecting after-current replaces a duration,
  and selecting a duration replaces after-current.
- Stopping playback explicitly clears the request, including stop actions from
  the player overlay, a remote controller, or platform media controls. A stop
  also invalidates pending item, album, episode, and stream negotiations, so
  their responses cannot resume stopped playback.
- Joining a playback group, changing profile, logging out, and quitting clear
  the request. The local timer is unavailable in a group and is never sent to a
  remote target.

The request is owned by the application, so navigating away from or rebuilding
the player overlay does not discard it. It is held only for the current process
and uses a monotonic elapsed clock, independent of playback position and wall
clock adjustments. It does not shut down or suspend the device. If the app event
loop is delayed, playback stops on the first callback that observes the elapsed
deadline, including a stream negotiation callback; device suspension and process
termination remain platform lifecycle behavior.

## Apple system controls and panels

- On macOS, **Appearance → macOS file and folder dialogs** is enabled by default. Turn it off to use the Qt Quick file and folder pickers. The preference applies the next time a picker opens.
- On macOS, **Appearance → macOS Navigate menu** adds Open Files, Search, and Settings to the system menu bar. It is off by default. The native commands use the existing QML shortcut handlers.
- **Appearance → System playback controls** adds Spool's Now Playing information and system transport callbacks. It is opt-in on macOS and enabled by default on iOS and tvOS. Turn it off to withdraw Now Playing information and system commands.

These preferences are stored independently on each installation. Linux offscreen checks exercise preference storage and the Qt Quick picker. The `macos-native-menu` selector exercises AppKit menu attachment, actions, and removal on hosted macOS. The `apple-media-controls` selector exercises MediaPlayer registration state and metadata removal on hosted macOS and in iOS and tvOS simulators. Verify native Cocoa panels on macOS and use physical Apple devices for Siri Remote gestures and end-to-end playback control from system surfaces. [Apple MediaPlayer remote interactions](https://developer.apple.com/documentation/avfoundation/supporting-remote-interactions-in-tvos)
