/**
 * Spool provider API 0.2.
 *
 * A provider is an ES module exporting `createSource(configuration, host)`.
 * Spool calls it once per account; the returned object's methods are the
 * operations below, each called as `operation(args, host)` and returning a
 * plain object or a Promise of one. Every operation is optional except
 * `describe`; implement what the service supports and declare it in
 * manifest.json `capabilities`.
 *
 * Runs on a worker thread in Qt's JS engine: ES2020 modules and Promises, no
 * Node or browser globals, no async/await. 500 ms of uninterrupted script
 * disables the module, and each operation must settle within 15 seconds.
 */

export type Value = null | boolean | number | string | Value[] | { [key: string]: Value };

export interface HttpOptions {
    method?: 'GET' | 'HEAD' | 'POST' | 'PUT' | 'PATCH' | 'DELETE';
    headers?: Record<string, string>;
    body?: string;
}
/** Redirects are not followed: `status` is 3xx and `location` is set. */
export interface HttpResponse { status: number; body: string; location?: string }

/** Authenticated generated download endpoint, or a range-capable media resource. */
export interface SpeedTestEndpoint {
    url: string; // HTTP(S); generated endpoints require {bytes} and {nonce}.
    /** Probe a media file of at least 4 MiB with native Range requests and strict HTTP 206 validation. */
    range?: boolean;
    headers?: Record<string, string>;
}
/** Conservative playback ceiling (bits/s), not raw link capacity. */
export interface SpeedTestResult { bitrate: number; parallelRequests: 1 | 2 | 4 }

export interface Socket {
    onopen: (() => void) | null;
    onmessage: ((text: string) => void) | null;
    onclose: ((code: number) => void) | null;
    send(text: string): void;
    close(): void;
}

export interface Device {
    /** Stable per install and per running instance; present it to servers. */
    id: string;
    name: string;
    app: 'Spool';
    version: string;
    platform: string;
    /** BCP 47. */
    locale: string;
}

/**
 * Given to createSource and lives as long as the account: use it for
 * connections that outlast an operation. Everything stops when the account
 * is removed, disabled or the provider is updated.
 */
export interface SourceHost {
    device: Device;
    /** Only origins the account was set up with (or manifest `origins`). */
    http(url: string, options?: HttpOptions): Promise<HttpResponse>;
    /** 0–60000 ms. */
    delay(milliseconds: number): Promise<void>;
    /** ws:// or wss:// on an allowed origin; at most four open. */
    socket(url: string, options?: { headers?: Record<string, string> }): Socket;
    /** Push to Spool: see Events. */
    emit<K extends keyof Events>(type: K, payload: Events[K]): void;
}

/** Given to each operation; its requests are cancelled with it. */
export interface OperationHost extends SourceHost {
    /** 0–10000 ms. */
    delay(milliseconds: number): Promise<void>;
    /** UDP broadcast on the local network; replies within `timeout` ms (100–5000). */
    discover(options: { port: number; message: string; timeout?: number }): Promise<{ address: string; text: string }[]>;
    /**
     * Measures on the native provider worker, discarding response bodies.
     * Same origin/TLS policy as http; no redirects or cookies. Cancelled with
     * this operation. Generated endpoints return exactly the requested bytes;
     * range endpoints must honor the native Range header and Content-Range.
     * Warms 512 KiB, then compares 4 MiB totals over one, two and optionally
     * four connections. Result reserves 25% headroom and is bounded to 1–1000 Mbps.
     */
    speedTest(endpoint: SpeedTestEndpoint): Promise<SpeedTestResult>;
}

/**
 * `configuration` is what the login screen completed with (empty while that
 * screen is being shown). Keep account state in the closure, not in module
 * globals: one module serves every account of this provider.
 */
export type CreateSource = (configuration: Record<string, Value>, host: SourceHost) => Source;

/** Throw `new Error('code')` with a short snake_case code; Spool shows no provider text. `http_401` marks the account as needing sign-in. */
export type Operation<A, R> = (args: A, host: OperationHost) => R | Promise<R>;

export interface Page { items: Item[]; cursor: string | null; total?: number | null; exhausted: boolean }
export interface PageArgs { cursor?: string; limit: number }

export type SortBy = 'SortName' | 'Random' | 'CommunityRating' | 'CriticRating' | 'DateCreated' | 'DateLastContentAdded'
    | 'OfficialRating' | 'PremiereDate' | 'PlayCount' | 'Runtime' | 'DatePlayed' | string;

/** The viewer's library filters; each is present only while set. */
export interface BrowseFilters {
    filters?: ('IsPlayed' | 'IsUnplayed' | 'IsFavorite' | 'IsResumable')[];
    genres?: string[]; years?: string[]; officialRatings?: string[]; tags?: string[]; studioIds?: string[];
    seriesStatus?: string[]; videoTypes?: string[]; includeItemTypes?: string[];
    isHd?: boolean; is4K?: boolean; is3D?: boolean; hasSubtitles?: boolean; hasTrailer?: boolean;
    hasSpecialFeature?: boolean; hasThemeSong?: boolean; hasThemeVideo?: boolean; specialEpisode?: boolean;
    isMissing?: boolean; isUnaired?: boolean;
    /** Titles starting with this letter, or '#' for anything before A. */
    alphabet?: string;
}

export interface Item {
    id: string;
    title: string;
    type: 'Movie' | 'Series' | 'Season' | 'Episode' | 'Audio' | 'MusicAlbum' | 'MusicArtist' | 'Playlist' | 'BoxSet'
        | 'Folder' | 'Video' | 'MusicVideo' | 'Book' | 'Photo' | 'PhotoAlbum' | 'TvChannel' | string;
    sortName?: string; overview?: string; year?: number;
    /** 100 ns ticks; send as a decimal string when beyond 2^53. */
    runtimeTicks?: number | string; resumeTicks?: number | string;
    favorite?: boolean; played?: boolean; playCount?: number; childCount?: number; virtual?: boolean;
    datePlayed?: string; dateCreated?: string; dateUpdated?: string; premiereDate?: string; endDate?: string; status?: string;
    seriesId?: string; seriesName?: string; seasonId?: string; season?: number; episode?: number;
    album?: string; albumId?: string; albumArtist?: string;
    /** Passed back as `{tag}` in the artwork template, or used as-is when it is an https URL and there is no template. */
    posterTag?: string; backdropTag?: string; logoTag?: string; bannerTag?: string; thumbTag?: string;
    seriesPosterTag?: string; albumPosterTag?: string;
    genres?: string[]; tags?: string[]; studios?: string[];
    officialRating?: string; communityRating?: number; criticRating?: number;
    externalIds?: Record<string, string>;
    links?: { name: string; url: string }[];
    people?: { id: string; name: string; type?: string; role?: string; imageTag?: string }[];
    variants?: Variant[];
}

export interface Variant {
    id: string; label?: string; container?: string; filename?: string;
    sizeBytes?: number | string; bitrate?: number; runtimeTicks?: number | string; streams?: Stream[];
}

export interface Stream {
    index: number; type: 'Video' | 'Audio' | 'Subtitle'; codec?: string; profile?: string; language?: string;
    title?: string; width?: number; height?: number; frameRate?: number; bitrate?: number; bitDepth?: number;
    channels?: number; sampleRate?: number; range?: string; rangeType?: string;
    default?: boolean; forced?: boolean; external?: boolean; interlaced?: boolean;
}

/** Merged into every resolve call by Spool. */
export interface PlaybackContext {
    /** The viewer's pick in the player; 0 is automatic. */
    maxBitrate: number; maxHeight: number;
    /** The standing preference from settings. */
    preferredMaxBitrate: number; preferredMaxHeight: number; preferRemux: boolean; unlimitedLocalNetwork: boolean;
    /** What this device decodes when `restrictVideoCodecs`; otherwise anything. */
    videoCodecs: string[]; restrictVideoCodecs: boolean;
    /** This account's measured conservative ceiling; zero until a successful idle probe. */
    measuredBitrate: number;
    /** Native playback range-request budget selected by the probe; two before measurement. */
    parallelRequests: 1 | 2 | 4;
}

export interface Resolved {
    url: string; headers?: Record<string, string>; variantId: string; playSessionId?: string;
    playMethod?: 'DirectPlay' | 'DirectStream' | 'Transcode'; container?: string;
    streams?: Stream[]; segments?: Segment[];
    trickplay?: { width: number; height: number; columns: number; rows: number; count: number; intervalMs: number };
}
/** Answer resolve with this to show the provider's `picker` screen first; Spool calls resolve again with what it completes with merged in. */
export interface PickRequest { pick: Record<string, Value> }

export interface Segment { type: 'Intro' | 'Outro' | 'Recap' | 'Preview' | 'Commercial'; startTicks: number | string; endTicks: number | string }

export interface Source {
    /** Required. Templates take {itemId} {type} {tag} {width} {height} {quality} {format}; trickplay {itemId} {width} {index} {variantId}. */
    describe(): { artwork?: string; trickplay?: string };

    libraries?: Operation<{}, { items: { id: string; title: string; collectionType?: string; posterTag?: string }[] }>;
    browse?: Operation<PageArgs & { parentId?: string; collectionType?: string; recursive?: boolean; genre?: string;
        studio?: string; sortBy?: SortBy; sortOrder?: 'Ascending' | 'Descending'; filters?: BrowseFilters }, Page>;
    items?: Operation<PageArgs & { ids: string[] }, Page>;
    search?: Operation<PageArgs & { query: string }, Page>;
    details?: Operation<{ itemId: string }, { item: Item }>;
    seasons?: Operation<PageArgs & { seriesId: string }, Page>;
    episodes?: Operation<PageArgs & { seriesId: string; seasonId?: string }, Page>;
    resume?: Operation<PageArgs, Page>;
    nextUp?: Operation<PageArgs, Page>;
    latest?: Operation<PageArgs & { parentId?: string }, Page>;
    similar?: Operation<PageArgs & { itemId: string }, Page>;
    personItems?: Operation<PageArgs & { personId: string }, Page>;
    filterOptions?: Operation<{ parentId: string; collectionType?: string },
        { genres?: string[]; years?: number[]; officialRatings?: string[]; tags?: string[] }>;

    resolve?: Operation<PlaybackContext & { itemId: string; variantId?: string; positionTicks: string; forceTranscode: boolean }, Resolved | PickRequest>;
    segments?: Operation<{ itemId: string }, { segments: Segment[] }>;
    /** Capability `speedTest`: supply your endpoint through host.speedTest(), never download test bodies in JS. */
    speedTest?: Operation<{}, SpeedTestResult>;
    report?: Operation<{ event: 'start' | 'progress' | 'stop'; itemId: string; variantId: string; playSessionId: string;
        playMethod: string; positionTicks: string; paused?: boolean; rate: number; volume?: number; muted?: boolean;
        failed?: boolean; audioStreamIndex: number; subtitleStreamIndex: number }, {}>;

    favorite?: Operation<{ itemId: string; value: boolean }, {}>;
    played?: Operation<{ itemId: string; value: boolean }, {}>;
    progress?: Operation<{ itemId: string; positionTicks: string }, {}>;

    /** Manifest `actions` are run here; `pick` shows the picker, then runs again with its result merged in. */
    runItemAction?: Operation<{ action: string; itemId: string; itemType: string; [choice: string]: Value },
        { changed?: boolean; itemId?: string; message?: string } | PickRequest>;

    /** Watching together (capability `groupPlayback`); state arrives as `group` events. */
    groups?: Operation<{}, { items: { id: string; name: string; participants: string[] }[] }>;
    groupCreate?: Operation<{ name: string }, {}>;
    groupJoin?: Operation<{ groupId: string }, {}>;
    groupLeave?: Operation<{}, {}>;
    groupSend?: Operation<GroupAction, {}>;
    /** Server clock for sync: when it received and when it answered, in ms since the epoch. */
    clock?: Operation<{}, { received: number; sent: number }>;

    /** Called before an account is removed. */
    signOut?: Operation<{}, {}>;

    /** Anything else is callable from the provider's own QML through `provider.request()`. */
    [operation: string]: unknown;
}

export type GroupAction =
    | { action: 'pause' | 'unpause' }
    | { action: 'seek'; positionTicks: string }
    | { action: 'next' | 'previous' | 'play'; entryId: string }
    | { action: 'setQueue'; itemIds: string[]; index: number; positionTicks: string }
    | { action: 'queue'; itemIds: string[]; next: boolean }
    | { action: 'move'; entryId: string; index: number }
    | { action: 'remove'; entryIds: string[] }
    | { action: 'buffering'; buffering: boolean; playing: boolean; positionTicks: string; entryId: string; at: number }
    | { action: 'ping'; ms: number };

/** `host.emit(type, payload)`. Times are server ms since the epoch. */
export interface Events {
    /** Something on the server changed; `itemId` narrows it. */
    changed: { itemId?: string };
    /** Merged into the stored configuration, e.g. a refreshed token. */
    configuration: Record<string, Value>;
    group:
        | { type: 'connected' }
        | { type: 'joined' | 'update'; groupId: string; name: string; state: string; reason?: string; participants: string[]; at?: number }
        | { type: 'participants'; participants: string[] }
        | { type: 'participantJoined' | 'participantLeft'; name: string }
        | { type: 'state'; state: 'Idle' | 'Waiting' | 'Paused' | 'Playing'; reason?: string }
        | { type: 'queue'; items: { itemId: string; entryId: string }[]; index: number; positionTicks: string; at?: number; reason?: string }
        | { type: 'command'; command: 'pause' | 'unpause' | 'seek' | 'stop'; at: number; positionTicks: string; entryId?: string; emittedAt?: number }
        | { type: 'left' }
        | { type: 'error'; code: 'group_missing' | 'access_denied' | 'create_denied' | 'join_denied' | 'disabled' | string };
    remote: RemoteCommand;
}

/** Another client asking this one to do something. */
export type RemoteCommand =
    | { command: 'play'; itemIds: string[]; index?: number; positionTicks?: string; mode?: 'now' | 'next' | 'last' | 'shuffle' }
    | { command: 'pause' | 'unpause' | 'playPause' | 'stop' | 'next' | 'previous' | 'rewind' | 'fastForward' | 'toggleMute' | 'stats' }
    | { command: 'seek'; positionTicks: string }
    | { command: 'volume'; value: number } | { command: 'volumeStep'; delta: number } | { command: 'mute'; value: boolean }
    | { command: 'audioTrack' | 'subtitleTrack'; index: number }
    | { command: 'repeat'; mode: 'RepeatNone' | 'RepeatAll' | 'RepeatOne' } | { command: 'shuffle'; value: boolean }
    | { command: 'quality'; bitrate: number; height?: number }
    | { command: 'navigate'; to: 'home' | 'search' | 'settings' | 'toggle-osd' | 'context-menu' | 'fullscreen' }
    | { command: 'show'; itemId: string; itemType?: string; title?: string }
    | { command: 'message'; text: string } | { command: 'text'; value: string }
    | { command: 'key'; name: 'up' | 'down' | 'left' | 'right' | 'pageUp' | 'pageDown' | 'select' | 'back' | 'home' | 'end' | 'space' };

/**
 * The object a provider screen (manifest `ui`) receives as its `provider`
 * property. Screens `import Spool` for Theme, Metrics, InputKeys and the
 * app's primitives (ActionButton, TextFieldRow, MenuRow, BusySpinner,
 * AppText, SecondaryText, MaterialIcon, IconButton, ProviderIcon, ...).
 */
export interface ScreenContext {
    role: 'login' | 'settings' | 'picker';
    /** For a picker: the `pick` object resolve or runItemAction returned. */
    arguments: Record<string, Value>;
    request(operation: string, args?: Record<string, Value>): Promise<Record<string, Value>>;
    /** Moves `items` into `rows` (a list model with `record` and `title` roles, up to 10,000 rows). */
    requestList(operation: string, args?: Record<string, Value>, append?: boolean): Promise<Record<string, Value>>;
    rows: unknown;
    /** Login only: let the account reach a server the viewer typed or picked. */
    allowOrigin(url: string): Promise<void>;
    /** login: { account, label, detail?, group?, configuration }; settings: { configuration? }; picker: the choice. */
    complete(result: Record<string, Value>): void;
    close(): void;
}
