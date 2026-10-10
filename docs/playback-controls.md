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
