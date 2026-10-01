// Only the native IPC boundary is simulated; index.html and main.ts are unchanged.
(() => {
    const scenario = new URLSearchParams(location.search).get('logs-test') ?? 'populated';
    if (scenario === 'browser') return;
    const callbacks = new Map();
    const listeners = new Map();
    const frames = new Map();
    const requestFrame = window.requestAnimationFrame.bind(window);
    let nextId = 0;
    let visible = false;
    const entries = scenario === 'empty' ? [] : [
        { id: 1, source: 'application', timestamp: 1800000000000, level: 'info', text: 'Startup application record' },
        { id: 2, source: 'localServer', timestamp: 1800000000001, level: 'warning', text: 'Startup local server record' },
    ];
    const state = window.logStartup = {
        calls: [],
        reads: 0,
        showCalls: 0,
        append() {
            entries.push({ ...entries[0], id: entries.length + 1, text: 'Live application record' });
        },
        visibility(value) {
            visible = value;
            document.dispatchEvent(new Event('visibilitychange'));
            for (const [id, event] of listeners) {
                if (event === 'logs-visibility') callbacks.get(id)?.({ event, payload: value });
            }
        },
    };
    Object.defineProperty(document, 'visibilityState', { configurable: true, get: () => visible ? 'visible' : 'hidden' });
    // Hidden native windows may not receive animation frames. Show must come first.
    window.requestAnimationFrame = (callback) => {
        if (visible) return requestFrame(callback);
        const id = ++nextId;
        frames.set(id, callback);
        return id;
    };
    window.__TAURI_INTERNALS__ = {
        metadata: {
            currentWindow: { label: 'logs' },
            get currentWebview() {
                if (scenario === 'scale-error') throw new Error('Scale adapter unavailable');
                return { label: 'logs' };
            },
        },
        transformCallback(callback) { const id = ++nextId; callbacks.set(id, callback); return id; },
        unregisterCallback(id) { callbacks.delete(id); },
        async invoke(command, args = {}) {
            state.calls.push(command);
            switch (command) {
                case 'desktop_interface_scale_mode':
                    if (scenario === 'preference-error') throw new Error('Display preference unavailable');
                    return 'auto';
                case 'diagnostic_log_level': return 'info';
                case 'plugin:log|log': return;
                case 'plugin:window|current_monitor': return null;
                case 'plugin:window|scale_factor': return 1;
                case 'plugin:window|is_maximized':
                case 'plugin:window|is_fullscreen': return false;
                case 'plugin:window|inner_size': return { width: 1100, height: 700 };
                case 'plugin:window|set_min_size':
                case 'plugin:window|set_size':
                case 'plugin:webview|set_webview_zoom': return;
                case 'plugin:window|show':
                    state.showCalls++;
                    state.visibility(true);
                    for (const callback of frames.values()) requestFrame(callback);
                    frames.clear();
                    return;
                case 'plugin:event|listen': listeners.set(args.handler, args.event); return args.handler;
                case 'plugin:event|unlisten': listeners.delete(args.eventId); return;
                case 'read_diagnostic_logs':
                    state.reads++;
                    if (scenario === 'read-error') throw new Error('Log files unavailable');
                    return { entries, total: entries.length, sequence: entries.length, newCount: 0, historyChanges: 0, olderCursor: null, newerCursor: null };
                default: throw new Error(`Unexpected Logs IPC: ${command}`);
            }
        },
    };
    window.__TAURI_EVENT_PLUGIN_INTERNALS__ = { unregisterListener(_event, id) { listeners.delete(id); } };
})();
