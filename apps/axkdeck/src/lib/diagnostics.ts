import { error as logError, info as logInfo, warn as logWarn } from '@tauri-apps/plugin-log';
import { invoke } from '@tauri-apps/api/core';

export type LogLevel = 'trace' | 'debug' | 'info' | 'warn' | 'error' | 'off';

let configuredLogLevel: LogLevel = 'info';

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
    void writeLog(logError, `${scope}: ${detail}`);
}

export function reportInfo(message: string): void {
    console.info(message);
    void writeLog(logInfo, message);
}

export type DiagnosticLevel = 'info' | 'warn' | 'error';

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
    const message = JSON.stringify({ ...fields, event });
    if (level === 'error') {
        console.error(message);
        void writeLog(logError, message);
    } else if (level === 'warn') {
        console.warn(message);
        void writeLog(logWarn, message);
    } else {
        console.info(message);
        void writeLog(logInfo, message);
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
            configuredLogLevel = await invoke<LogLevel>('diagnostic_log_level');
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
