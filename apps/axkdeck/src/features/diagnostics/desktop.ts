import { invoke } from '@tauri-apps/api/core';
import type { LogDriver } from './contracts';

export const desktopLogs: LogDriver = {
    read: (request) => invoke('read_diagnostic_logs', { request }),
    clear: () => invoke('clear_diagnostic_log_view'),
    save: (filter) => invoke('save_diagnostic_logs', { filter }),
};
