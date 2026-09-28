export function createSource(config, sourceHost) {
    const extensions = {'spool.collection-editing': 1, 'spool.item-actions': 1};
    const original = () => [
        {id: 'same', entryId: 'first', title: 'First occurrence', type: 'Movie'},
        {id: 'same', entryId: 'second', title: 'Second occurrence', type: 'Movie'},
        {id: 'third-media', entryId: 'third', title: 'Third', type: 'Movie'},
        {id: 'fourth-media', entryId: 'fourth', title: 'Fourth', type: 'Movie'},
        {id: 'fifth-media', entryId: 'fifth', title: 'Fifth', type: 'Movie'}
    ];
    let entries = original();
    let mode = '';
    let denied = false;
    let reads = [];
    let mutations = [];
    let actionReads = 0;
    let actionRuns = 0;
    const ensureContainer = args => {
        if (args.containerId !== 'list') throw new Error('wrong_container');
    };
    const mutate = (args, move) => {
        ensureContainer(args);
        mutations.push(args);
        const from = entries.findIndex(row => row.entryId === args.entryId);
        if (from < 0) throw new Error('not_found');
        if (mode === 'denied') {
            denied = true;
            throw new Error('http_403');
        }
        const removed = entries.splice(from, 1)[0];
        if (move) {
            const after = args.index === 0 ? null : entries[args.index - 1].entryId;
            if (after !== args.afterEntryId) throw new Error('inconsistent_destination');
            entries.splice(args.index, 0, removed);
        }
        if (mode === 'uncertain') throw new Error('network_lost_after_commit');
        return {};
    };
    return {
        describe() { return {extensions: extensions}; },
        setup(args) {
            entries = original();
            mode = args.mode || '';
            denied = false;
            reads = [];
            mutations = [];
            return {};
        },
        stats() { return {reads, mutations, entries, actionReads, actionRuns}; },
        collectionInfo(args) {
            ensureContainer(args);
            return {ordered: mode !== 'unordered', removable: !denied && mode !== 'smart',
                moveMode: mode === 'smart' || mode === 'unordered' ? 'none' : mode === 'after' ? 'after' : 'index'};
        },
        collectionEntries(args, host) {
            ensureContainer(args);
            reads.push(args.cursor === undefined ? null : args.cursor);
            const get = () => {
                if (mode === 'forever') return {items: [], cursor: 'p:' + reads.length, exhausted: false};
                if (mode === 'missing') return {items: [], exhausted: false};
                if (mode === 'repeat') return {items: [], cursor: 'again', exhausted: false};
                if (args.cursor === 'gap') return {items: [], cursor: 'p:2', exhausted: false};
                const offset = args.cursor === undefined ? 0 : Number(args.cursor.slice(2));
                const rows = entries.slice(offset, offset + 2);
                if (mode === 'duplicate-entry' && rows.length > 1) rows[1] = rows[0];
                const exhausted = offset + rows.length >= entries.length;
                return {items: rows, cursor: exhausted ? null : offset === 0 ? 'gap' : 'p:' + (offset + rows.length), exhausted};
            };
            return mode === 'slow' ? host.delay(100).then(get) : get();
        },
        collectionRemove(args) { return mutate(args, false); },
        collectionMove(args) { return mutate(args, true); },
        itemActions(args, host) {
            ++actionReads;
            if (args.containerId && args.containerId !== 'list') throw new Error('wrong_container');
            const result = {actions: [
                {id: 'allowed', label: 'Allowed'},
                {id: 'denied', label: 'Denied', enabled: false, reason: 'Read only'}
            ]};
            return mode === 'slow' ? host.delay(100).then(() => result) : result;
        },
        runItemAction(args) {
            if (args.action !== 'allowed') throw new Error('permission_denied');
            ++actionRuns;
            return {changed: true, itemId: args.itemId};
        },
        dropExtensions() { sourceHost.emit('extensionsChanged', {extensions: {}}); return {}; }
    };
}
