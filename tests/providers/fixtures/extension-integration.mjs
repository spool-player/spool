// Stateful loopback protocol used by the offscreen integration test. All
// operations cross ScriptRuntime's real HTTP boundary and account scopes.
export function createSource(config, host) {
    const capabilities = {settingsStorage: true, playbackPreferences: true,
        remoteTargets: true, accountActivation: true};
    let active = !config.protected;
    function call(operation, args, operationHost) {
        if (!active) throw new Error('account_locked');
        return operationHost.http(config.server + '/' + config.identity + '/' + operation, {
            method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(args)
        }).then(response => {
            if (response.status !== 200) throw new Error('fixture_unavailable');
            return JSON.parse(response.body);
        });
    }
    return {
        describe: () => ({capabilities, activation: config.protected
            ? {familyId: 'integration-family', identityId: config.identity} : undefined}),
        activate: args => {
            if (!config.protected) return {};
            if (!args.answers) return {pick: {kind: 'pin'}};
            if (args.answers.pin !== '1234') throw new Error('invalid_pin');
            active = true;
            return {};
        },
        preferencesRead: (args, h) => call('preferencesRead', args, h),
        preferencesWrite: (args, h) => call('preferencesWrite', args, h),
        dataInfo: (args, h) => call('dataInfo', args, h),
        dataRead: (args, h) => call('dataRead', args, h),
        dataWrite: (args, h) => call('dataWrite', args, h),
        remoteTargets: (args, h) => call('remoteTargets', args, h),
        remoteConnect: (args, h) => call('remoteState', args, h),
        remoteState: (args, h) => call('remoteState', args, h),
        remoteQueue: (args, h) => call('remoteQueue', args, h),
        remoteCommand: (args, h) => call('remoteCommand', args, h),
        stop: () => {}
    };
}
