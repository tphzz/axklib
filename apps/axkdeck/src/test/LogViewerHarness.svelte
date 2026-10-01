<script lang="ts">
    import LogViewer from '../features/diagnostics/LogViewer.svelte';
    import type { LogDriver, LogEntry, LogFilter, LogReadRequest } from '../features/diagnostics/contracts';
    let sequence = 0;
    const entries: LogEntry[] = [];
    const saves: (LogFilter | null)[] = [];
    const reads: LogReadRequest[] = [];
    let visibility: (visible: boolean) => void = () => undefined;
    function append(count = 1): void {
        for (let i = 0; i < count; i++) {
            const id = ++sequence;
            entries.push({
                id,
                source: id % 2 ? 'application' : 'localServer',
                timestamp: 1790575200000 + id,
                level: id % 3 === 0 ? 'warning' : 'info',
                text: `Entry ${id}: ${id % 3 === 0 ? 'failure observed' : 'operation completed'}${id % 7 === 0 ? '\n    stack frame at operation handler' : ''}${id % 11 === 0 ? ' long message'.repeat(30) : ''}`,
            });
        }
    }
    append(250);
    const driver: LogDriver = {
        async read(request) {
            reads.push(request);
            const levels = ['trace', 'debug', 'info', 'warning', 'error'];
            const matches = entries.filter(
                (entry) =>
                    (!request.filter.since || entry.id > request.filter.since) &&
                    (!request.filter.source || entry.source === request.filter.source) &&
                    (!request.filter.minimumLevel ||
                        levels.indexOf(entry.level!) >= levels.indexOf(request.filter.minimumLevel)) &&
                    entry.text.toLowerCase().includes(request.filter.search.toLowerCase()),
            );
            const end = request.before
                ? matches.findIndex((e) => e.id === request.before)
                : request.after
                  ? Math.min(matches.length, matches.findIndex((e) => e.id === request.after) + 101)
                  : matches.length;
            const start = Math.max(0, end - 100);
            return {
                entries: request.metadataOnly ? [] : matches.slice(start, end),
                total: matches.length,
                olderCursor: start > 0 ? matches[start].id : null,
                newerCursor: end < matches.length ? matches[end - 1].id : null,
                sequence,
                newCount: matches.filter((entry) => entry.id > request.knownThrough).length,
                historyChanges: 0,
            };
        },
        async clear() {
            return sequence;
        },
        async save(filter) {
            saves.push(filter);
            return null;
        },
    };
    Object.assign(window, {
        logFixture: { append, saves, reads, visibility: (visible: boolean) => visibility(visible) },
    });
</script>

<LogViewer
    {driver}
    subscribeVisibility={async (callback) => {
        visibility = callback;
        return () => {
            visibility = () => undefined;
        };
    }}
/>
