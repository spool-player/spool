/**
 * Spool current provider contract (manifest format 3).
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

/** Nonsecret host-approved identity for setup on one saved account/server. */
export type SetupContext = {
    accountId: string;
    serverId: string;
    serverName: string;
    serverOrigin: string;
    purpose: 'addProfile' | 'reconnect';
};

/** The complete set of current manifest and account capability names. */
export type Capability = 'search' | 'userState' | 'reporting' | 'segments' | 'groupPlayback'
    | 'remoteControl' | 'streamQuality' | 'trickplay' | 'speedTest' | 'downloads' | 'downloadTranscode'
    | 'discovery' | 'artworkOwners' | 'suggestions' | 'playbackPreferences' | 'settingsStorage'
    | 'itemActions' | 'collectionEditing' | 'playbackQueueReporting' | 'remoteTargets' | 'httpMetadata'
    | 'originGrants' | 'lanProbe' | 'accountActivation';
/** Strict boolean flags; missing or false account offers disable the capability. */
export type Capabilities = Readonly<Partial<Record<Capability, boolean>>>;

export interface HttpOptions {
    method?: 'GET' | 'HEAD' | 'POST' | 'PUT' | 'PATCH' | 'DELETE';
    headers?: Record<string, string>;
    body?: string;
    /** httpMetadata: at most 16 response header names; never Set-Cookie. */
    responseHeaders?: string[];
}
/** Redirects are not followed: `status` is 3xx and `location` is set. */
export interface HttpResponse {
    status: number; body: string; location?: string;
    /** Requested lowercase names only, bounded to 64 KiB; absent unless requested. */
    headers?: Record<string, string>;
}

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

export type LogLevel = 'trace' | 'debug' | 'info' | 'warn' | 'error';
/** Flat diagnostic metadata, never authentication, request bodies or media URLs. */
export type LogFields = Readonly<Record<string, null | boolean | number | string>>;

/**
 * The host surface shared by sources and operations, under different scopes:
 * source services outlive any one operation; operation requests are cancelled
 * with their call.
 */
export interface ProviderHost {
    device: Device;
    /** Frozen manifest declarations mapped to true; not account authorization. */
    readonly capabilities: Capabilities;
    /** Native category guard; check before constructing expensive diagnostic fields. */
    isLogEnabled(level: LogLevel): boolean;
    /** Lazy message runs only when enabled. Native redaction/bounds always apply. */
    log(level: LogLevel, message: string | (() => string), fields?: LogFields): void;
    /** Only origins the account was set up with (or manifest `origins`). */
    http(url: string, options?: HttpOptions): Promise<HttpResponse>;
    /** Push to Spool: see Events. */
    emit<K extends keyof Events>(type: K, payload: Events[K]): void;
}

/**
 * Given to createSource and lives as long as the account: use it for
 * connections that outlast an operation. Everything stops when the account
 * is removed, disabled or the provider is updated.
 */
export interface SourceHost extends ProviderHost {
    /** 0–60000 ms. */
    delay(milliseconds: number): Promise<void>;
    /** ws:// or wss:// on an allowed origin; at most four open. */
    socket(url: string, options?: { headers?: Record<string, string> }): Socket;
}

/**
 * Given to each operation; its requests are cancelled with it. Sockets are
 * source-level services only: operations cannot open them.
 */
export interface OperationHost extends ProviderHost {
    /** 0–10000 ms. */
    delay(milliseconds: number): Promise<void>;
    /** Requires discovery declaration. UDP broadcast; replies within `timeout` ms (100–5000). */
    discover(options: { port: number; message: string; timeout?: number }): Promise<{ address: string; text: string }[]>;
    /** lanProbe, login draft after allowLanDiscovery consent only.
     * No authentication, cookies, redirects or origin grants. Up to 32 targets,
     * four concurrent requests and 600 ms per page; bodies at most 4 KiB.
     */
    probeLocalHttp(options: { port: number; path: string; cursor?: string; limit?: number }): Promise<{
        responses: { origin: string; status: number; body: string }[];
        cursor: string | null; exhausted: boolean;
    }>;
    /**
     * Requires speedTest declaration. Measures on the native provider worker, discarding response bodies.
     * Same origin/TLS policy as http; no redirects or cookies. Cancelled with
     * this operation. Generated endpoints return exactly the requested bytes;
     * range endpoints must honor the native Range header and Content-Range.
     * Warms 512 KiB, then compares 4 MiB totals over one, two and optionally
     * four connections. Result reserves 25% headroom and is bounded to 1–1000 Mbps.
     */
    speedTest(endpoint: SpeedTestEndpoint): Promise<SpeedTestResult>;
}

/**
 * `configuration` is the account's private saved configuration. A targeted login
 * draft receives `{setupContext, setupAccount}` instead: the context is public
 * identity, while setupAccount is this provider's retained private configuration.
 * New login drafts have no saved account. Keep state in the source closure, not
 * module globals; never return setupAccount or credentials to QML.
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
    isHd?: boolean; is4K?: boolean; is3D?: boolean; isHdr?: boolean; hasSubtitles?: boolean; hasTrailer?: boolean;
    hasSpecialFeature?: boolean; hasThemeSong?: boolean; hasThemeVideo?: boolean; specialEpisode?: boolean;
    isMissing?: boolean; isUnaired?: boolean;
    /** Titles starting with this letter, or '#' for anything before A. */
    alphabet?: string;
}

export interface Item {
    id: string;
    /** Opaque occurrence identity within a collection; not a media ID. */
    entryId?: string;
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
    /** Owner of an inherited image; omitted for this item's own image. Scoped by the host like other item IDs. */
    backdropItemId?: string; thumbItemId?: string;
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
    /** An external subtitle file on the stream's own origin, fetched with its headers.
     *  External subtitles without one cannot be shown and are left out of the player. */
    url?: string;
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
    /** Global seek-preview preference, default true. When false, omit preview descriptors and skip preview-only metadata requests/prefetch. */
    videoPreviews: boolean;
}

export interface Resolved {
    url: string; headers?: Record<string, string>; variantId: string; playSessionId?: string;
    playMethod?: 'DirectPlay' | 'DirectStream' | 'Transcode'; container?: string;
    /** Source ticks represented by the normalized media stream's time-pos zero; omitted means zero.
     * Playback positions, reporting and segments remain in source coordinates.
     */
    timelineOriginTicks?: string;
    /** Fresh original-edition analysis, never the negotiated output or an automatic ceiling. */
    source?: { bitrate: number; width: number; height: number };
    streams?: Stream[]; segments?: Segment[];
    /** Native preview loader fetches/caches sheets or one BIF sequence using this account's headers. */
    trickplay?: { width: number; height: number; columns: number; rows: number; count: number; intervalMs: number;
        urlTemplate: string; format?: 'sprites'; headers?: Record<string, string> } |
        { format: 'bif'; url: string; width?: number; height?: number; headers?: Record<string, string> };
}
/** Answer resolve with this to show the provider's `picker` screen first; Spool calls resolve again with what it completes with merged in. */
export interface PickRequest { pick: Record<string, Value> }

/** Finite complete media file, original or encoded by the provider's server.
 * HLS/DASH manifests and live streams are not download endpoints.
 * All URLs must be approved account origins. Headers belong only to this file.
 */
export interface DownloadPlan {
    url: string;
    container: string;
    headers?: Record<string, string>;
    size?: number;
    /** Opaque server session cleanup; kept in memory, never persisted. */
    cleanup?: Record<string, Value>;
}
export interface DownloadArgs {
    itemId: string;
    mode: 'original' | 'transcoded';
    maxBitrate?: number;
    maxHeight?: number;
    variantId?: string;
}

export interface Segment { type: 'Intro' | 'Outro' | 'Recap' | 'Preview' | 'Commercial'; startTicks: number | string; endTicks: number | string }

// Optional catalogue operations.
export interface ItemAction {
    id: string; label: string; icon?: string; enabled?: boolean; reason?: string;
}
export interface CollectionInfo {
    ordered: boolean; removable: boolean; moveMode: 'none' | 'index' | 'after';
}
export interface PlaybackQueueSnapshot {
    revision: string;
    items: { itemId: string; entryId?: string; mediaType: 'audio' | 'video' }[];
}
export interface CatalogueOperations {
    /** suggestions: bounded recommendations, not Continue Watching. */
    suggestions?: Operation<PageArgs, Page>;
    /** itemActions: load on menu opening, not per rendered row. */
    itemActions?: Operation<{ itemId: string; itemType: string; containerId?: string; entryId?: string },
        { actions: ItemAction[] }>;
    /** collectionEditing: every returned row carries its container-local entryId. */
    collectionInfo?: Operation<{ containerId: string }, CollectionInfo>;
    collectionEntries?: Operation<PageArgs & { containerId: string }, Page>;
    collectionRemove?: Operation<{ containerId: string; entryId: string }, {}>;
    /** index and afterEntryId describe the same destination after removing the moving entry. */
    collectionMove?: Operation<{ containerId: string; entryId: string; index: number; afterEntryId: string | null }, {}>;
}

// Optional preference and application-data operations.
export interface PreferenceValues {
    /** ISO-639-2, or empty for no preference. */
    audioLanguage?: string;
    audioMode?: 'Default' | 'Smart';
    subtitleLanguage?: string;
    subtitleMode?: 'Default' | 'Smart' | 'OnlyForced' | 'Always' | 'None';
}
export interface PreferenceOperations {
    preferencesRead?: Operation<{}, { values: PreferenceValues; writable: (keyof PreferenceValues)[] }>;
    preferencesWrite?: Operation<{ values: Partial<PreferenceValues> }, {}>;
}
export interface ApplicationDataOperations {
    /** settingsStorage: maxBytes cannot exceed the host's 64 KiB/depth-16 bound. */
    dataInfo?: Operation<{}, { maxBytes: number; conditionalWrites: boolean }>;
    /** key is a canonical application-owned UUID, not a path; found distinguishes absent from null. */
    dataRead?: Operation<{ key: string }, { found: boolean; value?: Value; revision?: string }>;
    /** null expectedRevision is create-if-absent. Unsupported conditions must reject, not be ignored. */
    dataWrite?: Operation<{ key: string; value: Value; expectedRevision?: string | null }, { revision?: string }>;
    dataDelete?: Operation<{ key: string; expectedRevision?: string | null }, {}>;
}

// Optional outbound remote control, remoteTargets capability.
// This is independent of inbound Events.remote and remoteControl capability.
export interface RemoteTarget {
    id: string; name: string; detail?: string; origins?: string[]; commands: string[];
    queueEditing: 'none' | 'replace' | 'in-place'; customControls?: boolean;
}
export interface RemoteState {
    state: 'stopped' | 'playing' | 'paused' | 'buffering' | 'error';
    commands: string[];
    item?: Item;
    /** Decimal ticks; absence means unknown, not zero. */
    positionTicks?: string; runtimeTicks?: string;
    volume?: number; muted?: boolean; rate?: number;
    repeatMode?: 'RepeatNone' | 'RepeatAll' | 'RepeatOne'; shuffled?: boolean;
    audioTracks?: { id: string; label: string; selected: boolean }[];
    subtitleTracks?: { id: string; label: string; selected: boolean }[];
    queueRevision?: string; currentEntryId?: string;
    /** Genuine backend acknowledgment only; never a fabricated host sequence. */
    commandSequence?: number;
    /** Approved source origin; native decoding supports sprite sheets and whole BIF sequences. */
    preview?: Resolved['trickplay'];
}
export type RemoteTargetCommand =
    | { action: 'play'; itemIds: string[]; index: number; positionTicks: string;
        mode: 'now' | 'next' | 'last' | 'shuffle'; variantId?: string }
    | { action: 'pause' | 'unpause' | 'stop' | 'next' | 'previous' }
    | { action: 'seek'; positionTicks: string }
    | { action: 'volume'; value: number }
    | { action: 'mute' | 'shuffle'; value: boolean }
    /** null subtitles mean Off; null audio is invalid. IDs are provider track IDs, not indexes. */
    | { action: 'audioTrack' | 'subtitleTrack'; trackId: string | null }
    | { action: 'repeat'; mode: 'RepeatNone' | 'RepeatAll' | 'RepeatOne' }
    | { action: 'queuePlay' | 'queueRemove'; entryId: string }
    | { action: 'queueMove'; entryId: string; index: number; afterEntryId: string | null };
export interface RemoteTargetOperations {
    remoteTargets?: Operation<{}, { targets: RemoteTarget[] }>;
    /** Global preview preference is also supplied when inspecting another player. */
    remoteConnect?: Operation<{ targetId: string; videoPreviews: boolean }, RemoteState>;
    remoteState?: Operation<{ targetId: string; videoPreviews: boolean }, RemoteState>;
    remoteQueue?: Operation<PageArgs & { targetId: string }, Page>;
    remoteCommand?: Operation<{ targetId: string; command: RemoteTargetCommand }, { commandSequence?: number }>;
}

// Optional private account activation transaction, accountActivation capability.
export interface AccountActivationOperations {
    /** Native-only: provider QML cannot request this operation directly. */
    activate?: Operation<{
        reason: 'linked' | 'startup' | 'switch' | 'family';
        lastUsed: boolean;
        /** Opaque in-memory proof, at most 16 KiB; never persisted or exposed to QML. */
        grant?: Value;
        /** Picker submissions are nested and cannot replace the native-controlled fields above. */
        answers?: Record<string, Value>;
    }, { grant?: Value } | PickRequest>;
}

export interface Source extends CatalogueOperations, PreferenceOperations, ApplicationDataOperations,
    RemoteTargetOperations, AccountActivationOperations {
    /** Required. Artwork templates take {itemId} {type} {tag} {width} {height} {quality} {format}.
     * Offer every available declared capability, including baseline features.
     * Missing capabilities means no offers; undeclared flags cannot enable features.
     */
    describe(): { artwork?: string; capabilities?: Capabilities;
        activation?: { familyId: string; identityId: string } };

    libraries?: Operation<{}, { items: { id: string; title: string; collectionType?: string; posterTag?: string }[] }>;
    browse?: Operation<PageArgs & { parentId?: string; collectionType?: string; recursive?: boolean; genre?: string;
        studio?: string; sortBy?: SortBy; sortOrder?: 'Ascending' | 'Descending'; filters?: BrowseFilters }, Page>;
    items?: Operation<PageArgs & { ids: string[] }, Page>;
    search?: Operation<PageArgs & { query: string }, Page>;
    /** Omit preview-only metadata fields/prefetch when videoPreviews is false. */
    details?: Operation<{ itemId: string; videoPreviews: boolean }, { item: Item }>;
    seasons?: Operation<PageArgs & { seriesId: string }, Page>;
    episodes?: Operation<PageArgs & { seriesId: string; seasonId?: string }, Page>;
    resume?: Operation<PageArgs, Page>;
    nextUp?: Operation<PageArgs, Page>;
    latest?: Operation<PageArgs & { parentId?: string }, Page>;
    similar?: Operation<PageArgs & { itemId: string }, Page>;
    personItems?: Operation<PageArgs & { personId: string }, Page>;
    /** `supported` names the BrowseFilters this library honours, as a key or `key:value`
     *  (`filters:IsPlayed`). When present Spool offers only those; when absent it offers
     *  every filter except `isHdr`, which is offered only where declared. */
    filterOptions?: Operation<{ parentId: string; collectionType?: string },
        { genres?: string[]; years?: number[]; officialRatings?: string[]; tags?: string[]; supported?: string[] }>;

    resolve?: Operation<PlaybackContext & { itemId: string; variantId?: string; positionTicks: string; forceTranscode: boolean }, Resolved | PickRequest>;
    /** Capability downloads; downloadTranscode additionally allows mode=transcoded.
     * Negotiate only; native code streams the finite file without buffering it.
     * A PickRequest repeats this call with picker answers merged into the arguments.
     */
    download?: Operation<DownloadArgs, DownloadPlan | PickRequest>;
    /** Called on completion, cancellation or failure when a plan supplied cleanup. */
    downloadRelease?: Operation<{ cleanup: Record<string, Value> }, {}>;
    segments?: Operation<{ itemId: string }, { segments: Segment[] }>;
    /** Requires the declared and account-offered speedTest capability. */
    speedTest?: Operation<{}, SpeedTestResult>;
    report?: Operation<{ event: 'start' | 'progress' | 'stop'; itemId: string; variantId: string; playSessionId: string;
        playMethod: string; positionTicks: string; paused?: boolean; rate: number; volume?: number; muted?: boolean;
        failed?: boolean; audioStreamIndex: number; subtitleStreamIndex: number;
        /** playbackQueueReporting: immutable membership/order revision, omitted on unchanged progress. */
        queue?: PlaybackQueueSnapshot; queueIndex?: number }, {}>;

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
    /** Private credential update. Drafts retain it until successful setup; live
     * accounts persist it. Never forwarded to QML/account events or support reports. */
    configuration: Record<string, Value>;
    /** Replaces all account offers; true enables only declared flags.
     * Unknown keys or non-boolean values withdraw offers fail-closed.
     */
    capabilitiesChanged: { capabilities: Capabilities };
    /** Nonfatal playbackQueueReporting status; never credentials or raw server errors. */
    playbackQueueStatus: { revision: string; state: 'preparing' | 'ready' | 'unavailable' };
    /** Invalidates only this source/target; never an inbound remote command. */
    remoteChanged: { targetId: string };
    /** Account-activation-only device-local family options; never settings-sync input.
     * Providers must enforce who may change them. Values are booleans, not credentials or grants.
     */
    activationConfiguration: { configuration: Record<string, boolean> };
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
    /** PickRequest arguments, or login {setupContext:{accountId,serverId,serverName,serverOrigin,purpose}}.
     * Login hints never contain credentials; private setupAccount is factory-only. */
    arguments: Record<string, Value> & { setupContext?: SetupContext };
    /** Login draft declarations; live account effective flags; empty when closed. */
    readonly capabilities: Capabilities;
    /** Notifying, device-local boolean options for this activation family; no credentials/grants. */
    readonly activationConfiguration?: Readonly<Record<string, boolean>>;
    request(operation: string, args?: Record<string, Value>): Promise<Record<string, Value>>;
    /** Moves `items` into `rows` (a list model with `record` and `title` roles, up to 10,000 rows). */
    requestList(operation: string, args?: Record<string, Value>, append?: boolean): Promise<Record<string, Value>>;
    rows: unknown;
    /** Login server selection; existing accounts require originGrants and host consent. */
    allowOrigin(url: string): Promise<void>;
    /** lanProbe: explicit login-draft consent; does not authorize an origin. */
    allowLanDiscovery(): Promise<void>;
    /** Cancel this draft's pending discovery/consent without closing password login. */
    cancelLanDiscovery(): void;
    /** login: { account, label, detail?, group?, configuration? }; private credentials may arrive via draft configuration events.
     * settings: { configuration? }; picker: the choice. */
    complete(result: Record<string, Value>): void;
    close(): void;
}
