(function() {
        function promised(start) {
            return new Promise(function(resolve, reject) { start(resolve, reject); });
        }
        function sourceHost(bridge, device) {
            return Object.freeze({
                device: device,
                http: function(url, options) {
                    return promised(function(ok, no) { bridge.http(String(url), options || {}, ok, no); });
                },
                delay: function(ms) {
                    return promised(function(ok, no) { bridge.delay(ms | 0, ok, no); });
                },
                emit: function(type, payload) { bridge.emitEvent(String(type), payload); },
                socket: function(url, options) {
                    const socket = {onopen: null, onmessage: null, onclose: null};
                    const id = bridge.socket(String(url), (options && options.headers) || {},
                        function() { if (socket.onopen) socket.onopen(); },
                        function(text) { if (socket.onmessage) socket.onmessage(text); },
                        function(code) { if (socket.onclose) socket.onclose(code); });
                    if (!id)
                        throw new Error('socket_denied');
                    socket.send = function(text) { bridge.socketSend(id, String(text)); };
                    socket.close = function() { bridge.socketClose(id); };
                    return socket;
                }
            });
        }
        return {
            create: function(module, configuration, bridge, device) {
                return module.createSource(configuration, sourceHost(bridge, device));
            },
            call: function(source, method, args, operation, bridge, device) {
                const host = Object.freeze({
                    device: device,
                    http: function(url, options) {
                        return promised(function(ok, no) { operation.http(String(url), options || {}, ok, no); });
                    },
                    speedTest: function(options) {
                        if (!options || typeof options.url !== 'string')
                            return Promise.reject(new Error('request_denied'));
                        return promised(function(ok, no) { operation.speedTest(options, ok, no); });
                    },
                    delay: function(ms) {
                        if (!Number.isInteger(ms) || ms < 0 || ms > 10000)
                            return Promise.reject(new Error('timer_limit'));
                        return promised(function(ok, no) { operation.delay(ms, ok, no); });
                    },
                    discover: function(options) {
                        options = options || {};
                        return promised(function(ok, no) {
                            operation.discover(options.port | 0, String(options.message || ''),
                                options.timeout === undefined ? 1500 : options.timeout | 0, ok, no);
                        });
                    },
                    emit: function(type, payload) { bridge.emitEvent(String(type), payload); }
                });
                try {
                    Promise.resolve(source[method](args, host)).then(
                        function(value) { operation.resolve(value); },
                        function(error) { operation.reject(error); });
                } catch (error) { operation.reject(error); }
            }
        };
    })()