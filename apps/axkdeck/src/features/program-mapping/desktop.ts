import { invoke } from '@tauri-apps/api/core';
import { listen } from '@tauri-apps/api/event';
import type { MappingCommand, MappingHostAdapter, MappingMessage } from './protocol';

export const mappingHostAdapter: MappingHostAdapter = {
    open: () => invoke('open_program_mapping'),
    publish: (message) => invoke('publish_program_mapping', { message }),
    listen: (callback) => listen<MappingCommand>('program-mapping-command', (event) => callback(event.payload)),
};
export const mappingWindowAdapter = {
    send: (command: MappingCommand): Promise<void> => invoke('command_program_mapping', { command }),
    listen: (callback: (message: MappingMessage) => void) =>
        listen<MappingMessage>('program-mapping-state', (event) => callback(event.payload)),
};
