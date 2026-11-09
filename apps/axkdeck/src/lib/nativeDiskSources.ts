import { invoke } from '@tauri-apps/api/core';
import type { ClientUploadSource } from './clientUploadSource';
import { nativeFileSource } from './nativeFileSource';
import { tx16wDiskExtensions, tx16wDiskMediaType } from './tx16wImport';

interface NativeDiskDescriptor {
    path: string;
    identity: string;
    size: number;
}

async function sourceFromDescriptor(descriptor: NativeDiskDescriptor): Promise<ClientUploadSource> {
    const source = await nativeFileSource(descriptor.path, tx16wDiskExtensions, tx16wDiskMediaType(descriptor.path)!);
    if (source.size !== descriptor.size) throw new Error(`Selected disk changed during admission: ${source.name}`);
    return {
        ...source,
        identity: descriptor.identity,
        nearbySources: async (signal) => {
            signal.throwIfAborted();
            const files = await invoke<NativeDiskDescriptor[]>('nearby_native_disk_sources', { path: descriptor.path });
            signal.throwIfAborted();
            const sources: ClientUploadSource[] = [];
            for (const file of files) {
                signal.throwIfAborted();
                sources.push(await sourceFromDescriptor(file));
            }
            signal.throwIfAborted();
            return sources;
        },
    };
}

export async function nativeDiskSource(path: string): Promise<ClientUploadSource> {
    return sourceFromDescriptor(await invoke<NativeDiskDescriptor>('inspect_native_disk_source', { path }));
}

export async function selectNativeDiskSources(): Promise<ClientUploadSource[]> {
    const selected = await invoke<NativeDiskDescriptor[]>('select_native_disk_sources');
    return Promise.all(selected.map(sourceFromDescriptor));
}
