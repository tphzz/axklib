import { invoke } from '@tauri-apps/api/core';
import { listen } from '@tauri-apps/api/event';
import type { MappingCommand, MappingHostAdapter, MappingMessage, MappingRole } from './protocol';

export const mappingHostAdapter = (role: MappingRole): MappingHostAdapter => ({
    open: () => invoke('open_mapping_editor', { role }),
    publish: (message) => invoke('publish_mapping_editor', { role, message }),
    listen: (callback) =>
        listen<MappingCommand>('mapping-editor-command', (event) => {
            if (event.payload.role === role) callback(event.payload);
        }),
});
export const mappingWindowAdapter = {
    send: (command: MappingCommand): Promise<void> => invoke('command_mapping_editor', { command }),
    listen: (callback: (message: MappingMessage) => void) =>
        listen<MappingMessage>('mapping-editor-state', (event) => callback(event.payload)),
};
