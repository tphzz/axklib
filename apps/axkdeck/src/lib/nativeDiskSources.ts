import { invoke } from '@tauri-apps/api/core';
import type { ClientUploadSource } from './clientUploadSource';
import { nativeFileSource } from './nativeFileSource';
import { tx16wDiskExtensions, tx16wDiskMediaType } from './tx16wImport';

interface NativeDiskDescriptor {
    path: string;
    identity: string;
    size: number;
    sha256: string;
}
const descriptors = new WeakMap<ClientUploadSource['readChunk'], NativeDiskDescriptor>();
let nextScan = 0;

async function sourceFromDescriptor(descriptor: NativeDiskDescriptor): Promise<ClientUploadSource> {
    const source = await nativeFileSource(descriptor.path, tx16wDiskExtensions, tx16wDiskMediaType(descriptor.path)!);
    if (source.size !== descriptor.size) throw new Error(`Selected disk changed during admission: ${source.name}`);
    descriptors.set(source.readChunk, descriptor);
    return {
        ...source,
        identity: descriptor.identity,
        sha256: descriptor.sha256,
        nearbySources: async (selected, excluded, signal) => {
            signal.throwIfAborted();
            const scanId = ++nextScan;
            const cancel = () => {
                void invoke('cancel_native_companion_scan', { scanId }).catch(() => undefined);
            };
            signal.addEventListener('abort', cancel, { once: true });
            try {
                const files = await invoke<{ sources: NativeDiskDescriptor[]; ambiguous: string[] }>(
                    'resolve_native_disk_companions',
                    {
                        scanId,
                        selected: selected.map((file) => {
                            const selectedDescriptor = descriptors.get(file.readChunk);
                            if (!selectedDescriptor) throw new Error('Select native disks to search for companions');
                            return selectedDescriptor;
                        }),
                        excluded,
                    },
                );
                signal.throwIfAborted();
                const sources: ClientUploadSource[] = [];
                for (const file of files.sources) {
                    signal.throwIfAborted();
                    sources.push(await sourceFromDescriptor(file));
                }
                signal.throwIfAborted();
                return { sources, ambiguous: files.ambiguous };
            } finally {
                signal.removeEventListener('abort', cancel);
            }
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
