import { invoke } from '@tauri-apps/api/core';
import type { FilesystemExportActions } from './filesystemExport';

export const nativeFilesDrag: NonNullable<FilesystemExportActions['drag']> = {
    reserve: (expectedSize) => invoke<string>('reserve_native_files_drag', { expectedSize }),
    prepare: (ticket, contentPath) => invoke<void>('prepare_native_files_drag', { ticket, contentPath }),
    start: (ticket) => invoke<void>('start_native_files_drag', { ticket }),
    cancel: (ticket) => invoke<void>('cancel_native_files_drag', { ticket }),
};
