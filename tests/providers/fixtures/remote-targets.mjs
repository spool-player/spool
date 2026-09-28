export function createSource(config, sourceHost) {
    const extensions = {'spool.remote-targets': 1, 'spool.origin-grants': 1};
    const actions = ['play', 'pause', 'unpause', 'stop', 'seek', 'volume', 'mute', 'shuffle', 'repeat',
        'audioTrack', 'subtitleTrack', 'queuePlay', 'queueRemove', 'queueMove', 'next', 'previous'];
    let settings = {};
    let connects = [];
    let commands = [];
    let stateReads = [];
    let queueReads = [];
    let lists = 0;
    let sequence = 0;
    let revision = 0;
    const original = () => [
        {id: 'movie', entryId: 'first', title: 'First', type: 'Movie'},
        {id: 'movie', entryId: 'second', title: 'Second', type: 'Movie'},
        {id: 'other', entryId: 'third', title: 'Third', type: 'Movie'}
    ];
    let entries = original();
    let states = {};
    const initial = id => ({state: 'playing', commands: actions.slice(),
        item: {id: id + '-movie', title: id, type: 'Movie'}, positionTicks: '10000000',
        runtimeTicks: '9223372036854775807',
        audioTracks: [{id: 'audio-id', label: 'English', selected: true}],
        subtitleTracks: [{id: 'subtitle-id', label: 'English', selected: true}],
        currentEntryId: 'first'});
    const snapshot = id => {
        if (settings.gone) throw new Error('target_unavailable');
        const state = JSON.parse(JSON.stringify(states[id] || initial(id)));
        if (!settings.noRevision) state.queueRevision = String(revision);
        if (settings.ack) state.commandSequence = settings.staleAck ? 0 : sequence;
        if (settings.position !== undefined) state.positionTicks = settings.position;
        if (settings.badTracks) state.audioTracks = Array.from({length: 129}, (_, n) => ({id: String(n), label: 'Track', selected: false}));
        if (settings.unsupported) state.commands = ['stop'];
        if (settings.playUnavailable) state.commands = state.commands.filter(name => name !== 'play');
        if (settings.unknownFields) {
            state.positionTicks = undefined;
            state.runtimeTicks = undefined;
            state.volume = undefined;
            state.preview = undefined;
        }
        if (settings.preview) state.preview = {width: 160, height: 90, columns: 10, rows: 10,
            count: 300, intervalMs: 1000, urlTemplate: settings.preview};
        return state;
    };
    const ensure = args => {
        if (args.targetId !== 'a' && args.targetId !== 'b') throw new Error('wrong_target');
    };
    return {
        describe() { return {extensions}; },
        setup(args) {
            settings = args;
            connects = []; commands = []; stateReads = []; queueReads = []; lists = 0;
            sequence = 0; revision = 0; entries = original(); states = {};
            return {};
        },
        configure(args) { Object.assign(settings, args); return {}; },
        stats() { return {connects, commands, stateReads, queueReads, lists, entries}; },
        publish(args) {
            if (args.state) states[args.targetId || 'a'] = Object.assign(initial(args.targetId || 'a'), args.state);
            if (args.revision) ++revision;
            const targetId = args.targetId || 'a';
            for (let n = 0; n < (args.events || 1); ++n) sourceHost.emit('remoteChanged', {targetId});
            return {};
        },
        remoteTargets(args, host) {
            ++lists;
            sourceHost.emit('fixtureRemoteList', {active: true});
            const get = () => {
                sourceHost.emit('fixtureRemoteList', {active: false});
                const target = id => ({id, name: id, detail: 'Fixture target', commands: actions,
                    origins: id === 'a' && settings.origin ? [settings.origin, 'https://never-grant.invalid'] : [],
                    queueEditing: settings.readOnly ? 'none' : 'replace', customControls: true});
                if (settings.tooMany) return {targets: Array.from({length: 129}, (_, n) => target(String(n)))};
                return {targets: settings.disappear ? [] : [target('a'), target('b'), target('self-device')]};
            };
            return settings.listDelay ? host.delay(settings.listDelay).then(get) : get();
        },
        remoteConnect(args, host) {
            ensure(args);
            connects.push(args.targetId);
            const result = snapshot(args.targetId);
            return settings.connectDelay ? host.delay(settings.connectDelay).then(() => result) : result;
        },
        remoteState(args, host) {
            ensure(args);
            stateReads.push(args.targetId);
            const result = snapshot(args.targetId);
            return settings.stateDelay ? host.delay(settings.stateDelay).then(() => result) : result;
        },
        remoteCommand(args, host) {
            ensure(args);
            const command = args.command;
            commands.push(command);
            if (command.action === 'play' && settings.failPlay) {
                settings.playUnavailable = true;
                throw new Error('remote_play_unavailable');
            }
            const state = states[args.targetId] || initial(args.targetId);
            if (!settings.ignoreCommands) {
                if (command.action === 'pause') state.state = 'paused';
                if (command.action === 'unpause') state.state = 'playing';
                if (command.action === 'stop') state.state = 'stopped';
                if (command.action === 'seek') state.positionTicks = command.positionTicks;
                if (command.action === 'volume') state.volume = command.value;
                if (command.action === 'mute') state.muted = command.value;
                if (command.action === 'shuffle') state.shuffled = command.value;
                if (command.action === 'repeat') state.repeatMode = command.mode;
                if (command.action === 'play') {
                    state.item = {id: command.itemIds[command.index], title: 'Started remotely', type: 'Movie'};
                    state.positionTicks = command.positionTicks;
                    state.state = 'playing';
                }
            }
            if (command.action === 'queueRemove') {
                entries = entries.filter(item => item.entryId !== command.entryId);
                ++revision;
            }
            if (command.action === 'queueMove') {
                const from = entries.findIndex(item => item.entryId === command.entryId);
                const item = entries.splice(from, 1)[0];
                const expected = command.index ? entries[command.index - 1].entryId : null;
                if (expected !== command.afterEntryId) throw new Error('wrong_anchor');
                entries.splice(command.index, 0, item);
                ++revision;
            }
            states[args.targetId] = state;
            ++sequence;
            const result = settings.ack ? {commandSequence: sequence} : {};
            const finish = () => {
                if (settings.uncertain) throw new Error('network_lost_after_commit');
                return result;
            };
            return settings.commandDelay ? host.delay(settings.commandDelay).then(finish) : finish();
        },
        remoteQueue(args, host) {
            ensure(args);
            queueReads.push(args.cursor === undefined ? null : args.cursor);
            const get = () => {
                if (settings.forever) return {items: [], cursor: 'empty:' + queueReads.length, exhausted: false};
                if (settings.badCursor) return {items: [], cursor: 'repeat', exhausted: false};
                if (args.cursor === 'gap') return {items: [], cursor: 'p:2', exhausted: false};
                const offset = args.cursor === undefined ? 0 : Number(args.cursor.slice(2));
                const items = entries.slice(offset, offset + 2);
                if (settings.badEntries && items.length > 1) items[1] = items[0];
                const exhausted = offset + items.length >= entries.length;
                return {items, cursor: exhausted ? null : offset === 0 ? 'gap' : 'p:' + (offset + items.length), exhausted};
            };
            return settings.queueDelay ? host.delay(settings.queueDelay).then(get) : get();
        },
        dropExtensions() { sourceHost.emit('extensionsChanged', {extensions: {}}); return {}; }
    };
}
