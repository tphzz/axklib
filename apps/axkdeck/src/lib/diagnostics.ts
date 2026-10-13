import {
    error as logError,
    info as logInfo,
    warn as logWarn,
    debug as logDebug,
    trace as logTrace,
} from '@tauri-apps/plugin-log';
import { invoke } from '@tauri-apps/api/core';
import { listen } from '@tauri-apps/api/event';

export type LogLevel = 'trace' | 'debug' | 'info' | 'warn' | 'error' | 'off';

let configuredLogLevel: LogLevel = 'info';
let captureRevision = 0;

async function refreshCapture(): Promise<void> {
    const revision = ++captureRevision;
    const level = await invoke<LogLevel>('diagnostic_log_level');
    if (revision === captureRevision) configuredLogLevel = level;
}

function tauriAvailable(): boolean {
    return '__TAURI_INTERNALS__' in window;
}

function describe(reason: unknown): string {
    if (reason instanceof Error) return `${reason.name}: ${reason.message}${reason.stack ? `\n${reason.stack}` : ''}`;
    return String(reason);
}

async function writeLog(writer: (message: string) => Promise<void>, message: string): Promise<void> {
    if (!tauriAvailable()) return;
    try {
        await writer(message);
    } catch (reason) {
        console.warn('Unable to write the desktop log', reason);
    }
}

export function reportError(scope: string, reason: unknown): void {
    const detail = describe(reason);
    console.error(scope, reason);
    void writeLog(
        logError,
        JSON.stringify({ event: 'frontend_error', source: 'frontend', level: 'error', scope, message: detail }),
    );
}

export function reportInfo(message: string): void {
    console.info(message);
    void writeLog(logInfo, JSON.stringify({ event: 'frontend_info', source: 'frontend', level: 'info', message }));
}

export type DiagnosticLevel = 'trace' | 'debug' | 'info' | 'warn' | 'error';

export function diagnosticsEnabled(): boolean {
    return configuredLogLevel === 'debug' || configuredLogLevel === 'trace';
}

export function audioDiagnosticsEnabled(): boolean {
    return diagnosticsEnabled();
}

export function reportDiagnostic(
    event: string,
    fields: Readonly<Record<string, unknown>> = {},
    level: DiagnosticLevel = 'info',
): void {
    if ((level === 'debug' && !diagnosticsEnabled()) || (level === 'trace' && configuredLogLevel !== 'trace')) return;
    const message = JSON.stringify({
        source: 'frontend',
        ...fields,
        event,
        level: level === 'warn' ? 'warning' : level,
    });
    if (level === 'error') {
        console.error(message);
        void writeLog(logError, message);
    } else if (level === 'warn') {
        console.warn(message);
        void writeLog(logWarn, message);
    } else if (level === 'debug' || level === 'trace') {
        console.debug(message);
        void writeLog(level === 'trace' ? logTrace : logDebug, message);
    } else {
        console.info(message);
        void writeLog(logInfo, message);
    }
}

export function reportMutationTiming(operation: string, started: number, itemCount: number): void {
    if (!diagnosticsEnabled()) return;
    reportDiagnostic(
        'image_mutation_completed',
        {
            operation,
            itemCount,
            durationMs: Math.round(performance.now() - started),
            strategy: 'journaled-in-place',
        },
        'debug',
    );
}

export async function measureDiagnostic<T>(
    event: string,
    phase: string,
    fields: () => Readonly<Record<string, unknown>>,
    action: () => Promise<T>,
): Promise<T> {
    if (!diagnosticsEnabled()) return action();
    const started = performance.now();
    let outcome = 'resolved';
    try {
        return await action();
    } catch (error) {
        outcome = 'rejected';
        throw error;
    } finally {
        try {
            reportDiagnostic(
                event,
                { ...fields(), phase, outcome, durationMs: Math.round(performance.now() - started) },
                'debug',
            );
        } catch {
            /* Diagnostics must not affect a write or its recovery state. */
        }
    }
}

export async function installDiagnostics(): Promise<void> {
    window.addEventListener('error', (event) => {
        reportError('Unhandled frontend error', event.error ?? event.message);
    });
    window.addEventListener('unhandledrejection', (event) => {
        reportError('Unhandled frontend promise rejection', event.reason);
    });

    if (tauriAvailable()) {
        try {
            const stop = await listen('diagnostic-capture-changed', () => {
                void refreshCapture().catch((reason) => console.warn('Unable to refresh diagnostic capture', reason));
            });
            window.addEventListener('pagehide', stop, { once: true });
            await refreshCapture();
        } catch (reason) {
            console.warn('Unable to read the desktop log level; using info', reason);
        }
    }

    if (import.meta.env.DEV && tauriAvailable()) {
        window.addEventListener('keydown', (event) => {
            if (event.key !== 'F12') return;
            event.preventDefault();
            void invoke('open_developer_tools').catch((reason) => reportError('Open developer tools failed', reason));
        });
        reportInfo('Development diagnostics are active; press F12 to open developer tools.');
    }
}
