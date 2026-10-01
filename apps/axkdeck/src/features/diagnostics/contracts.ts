export type LogSource = 'application' | 'localServer';
export type LogLevel = 'trace' | 'debug' | 'info' | 'warning' | 'error';
export interface LogEntry {
    id: number;
    source: LogSource;
    timestamp: number | null;
    level: LogLevel | null;
    text: string;
}
export interface LogFilter {
    source: LogSource | null;
    minimumLevel: LogLevel | null;
    search: string;
    includeUnclassified: boolean;
    since: number | null;
}
export interface LogPage {
    entries: LogEntry[];
    olderCursor: number | null;
    newerCursor: number | null;
    total: number;
    newCount: number;
    sequence: number;
    historyChanges: number;
}
export interface LogReadRequest {
    filter: LogFilter;
    before: number | null;
    after: number | null;
    knownThrough: number;
    metadataOnly: boolean;
}
export interface LogDriver {
    read(request: LogReadRequest): Promise<LogPage>;
    clear(): Promise<number>;
    save(filter: LogFilter | null): Promise<{ path: string; warning: string | null } | null>;
}
export const defaultLogFilter = (): LogFilter => ({
    source: null,
    minimumLevel: null,
    search: '',
    includeUnclassified: true,
    since: null,
});
