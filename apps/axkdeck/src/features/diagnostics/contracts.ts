export const logSources = [
    'application',
    'frontend',
    'http',
    'jobs',
    'filesystem',
    'objects',
    'devices',
    'audio',
    'other',
] as const;
export type LogSource = (typeof logSources)[number];
export const logLevels = ['error', 'warning', 'info', 'debug', 'trace'] as const;
export type LogLevel = (typeof logLevels)[number];
export const includesLevel = (limit: LogLevel, level: LogLevel): boolean =>
    logLevels.indexOf(level) <= logLevels.indexOf(limit);
export interface CaptureState {
    level: LogLevel;
    localServer: 'ready' | 'unavailable' | 'remote';
    error: string | null;
}
export interface LogEntry {
    id: number;
    source: LogSource;
    origin: 'application' | 'localServer';
    timestamp: number | null;
    level: LogLevel;
    text: string;
}
export interface LogFilter {
    sources: LogSource[];
    level: LogLevel;
    search: string;
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
    capture(): Promise<CaptureState>;
    setCapture(level: LogLevel): Promise<CaptureState>;
}
export const defaultLogFilter = (): LogFilter => ({
    sources: [...logSources],
    level: 'info',
    search: '',
    since: null,
});
