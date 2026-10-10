// Stateful replacement/CAS storage and independent native preferences. All
// mutations, delayed snapshots and competing-client writes share server state.
export function createSource(config, sourceHost) {
    const clone = value => JSON.parse(JSON.stringify(value));
    let doc = config.document || {format: 1, entries: {}};
    let found = config.found !== false;
    let revision = 1;
    let native = config.native || {audioLanguage: 'eng', audioMode: 'Default', subtitleLanguage: '', subtitleMode: 'Default'};
    let writable = config.writable || ['audioLanguage', 'audioMode', 'subtitleLanguage', 'subtitleMode'];
    let options = {};
    let storageReads = 0, preferenceReads = 0;
    let writes = [], preferenceWrites = [];
    const pendingResponses = {};
    const capabilities = config.capabilities || {playbackPreferences: true, settingsStorage: true};
    const delay = (host, ms, result) => ms ? host.delay(ms).then(() => result) : result;
    const respond = (operation, host, ms, result) => {
        if ((options.blockedOperations || []).indexOf(operation) < 0)
            return delay(host, ms, result);
        return new Promise(resolve => {
            if (!pendingResponses[operation]) pendingResponses[operation] = [];
            pendingResponses[operation].push(() => resolve(delay(host, ms, result)));
        });
    };
    const fail = () => { if (options.offline) throw new Error('network_offline'); };
    return {
        describe(args, host) { return delay(host, config.describeDelay || 0, {capabilities}); },
        configure(args) {
            if (args.document !== undefined) { doc = clone(args.document); found = true; ++revision; }
            if (args.found !== undefined) found = args.found;
            if (args.native !== undefined) native = clone(args.native);
            if (args.writable !== undefined) writable = clone(args.writable);
            options = Object.assign(options, args.options || {});
            for (const operation of Object.keys(pendingResponses)) {
                if ((options.blockedOperations || []).indexOf(operation) >= 0) continue;
                const responses = pendingResponses[operation];
                delete pendingResponses[operation];
                for (const release of responses) release();
            }
            return {};
        },
        stats() {
            const blockedResponses = {};
            for (const operation of Object.keys(pendingResponses))
                blockedResponses[operation] = pendingResponses[operation].length;
            return {document: doc, native, writes, preferenceWrites, storageReads, preferenceReads, revision, blockedResponses};
        },
        dataInfo() { return {maxBytes: config.maxBytes || 65536, conditionalWrites: !!config.cas}; },
        dataRead(args, host) {
            fail(); ++storageReads;
            const snapshot = found ? {found: true, value: clone(doc), revision: String(revision)} : {found: false};
            return respond('dataRead', host, options.readDelay || 0, snapshot);
        },
        dataWrite(args, host) {
            fail();
            if (config.cas && options.conflictOnce) {
                options.conflictOnce = false;
                if (options.concurrentDocument) doc = clone(options.concurrentDocument);
                ++revision;
                throw new Error('conflict');
            }
            if (config.cas && (args.expectedRevision === null ? found : args.expectedRevision !== String(revision)))
                throw new Error('conflict');
            if (!config.cas && args.expectedRevision !== undefined) throw new Error('unsupported_condition');
            writes.push(clone(args));
            doc = clone(args.value); found = true; ++revision;
            if (options.overwriteCount) {
                --options.overwriteCount;
                doc = clone(options.concurrentDocument || {format: 1, entries: {}});
                ++revision;
            }
            return respond('dataWrite', host, options.writeDelay || 0, config.cas ? {revision: String(revision)} : {});
        },
        preferencesRead(args, host) {
            if (options.preferencesUnavailable) throw new Error('preferences_unavailable');
            fail(); ++preferenceReads;
            return delay(host, options.nativeReadDelay || 0, {values: clone(native), writable: clone(writable)});
        },
        preferencesWrite(args, host) {
            fail();
            for (const key of Object.keys(args.values))
                if (writable.indexOf(key) < 0) throw new Error('permission_denied');
            preferenceWrites.push(clone(args.values));
            native = Object.assign({}, native, clone(args.values));
            return respond('preferencesWrite', host, options.nativeWriteDelay || 0, {});
        },
        dropCapabilities() { sourceHost.emit('capabilitiesChanged', {capabilities: {}}); return {}; }
    };
}
