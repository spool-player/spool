# Trailers and extras

Movie, series, season, and episode details show **Trailers** and **Extras** when
the media source has related media for that item. Select a card to play it;
hold the remote's select button or open the item menu to queue it, view its
media information, or update its watched/favourite state.

These shelves come from the provider's related-media catalog contract
(`trailers` and `extras` kinds), keeping server endpoints inside the provider.
They contain separately indexed media items and use the normal authenticated
playback path, including stream negotiation, subtitles, remote targets, and
resume reporting. Watching an extra records progress on that extra, leaving
the main feature's progress intact. Nothing plays automatically before a movie.

Empty shelves are hidden. If either optional request fails, the main feature
and any other successfully loaded rows remain usable. Navigation to another
item or switching accounts invalidates outstanding shelf results.

This feature covers media indexed by the source. Remote trailer web links,
theme-video backgrounds, and a cinema pre-roll mode are separate additions.
