import type { ImageTransport } from '../lib/transport';
import type { DirectoryRef, SandboxEntry } from '../lib/storageLocations';

export const storagePickerDirectory: DirectoryRef = { rootId: 'workspace', relativePath: 'Drum Kits' };

export const storagePickerNames = [
    'ACE-FR2L',
    'ACE-tone',
    'Alesis-DM5',
    'Alesis-HR16',
    'BOSS-Dr110',
    'industrialkit',
    'norddrms',
    'ROKTON-factorykit',
    'ROKTON-ripped',
    'Roland-DDR30',
    'TR606',
    'TR707',
    'TR909',
    "Virus-SFX'n-drums",
    'Disk02.hds',
    'disk2.hds',
    'Disk10.hds',
];

export function storagePickerFixture(longList = false): ImageTransport {
    const names = longList ? Array.from({ length: 90 }, (_, index) => `Disk${index + 1}.hds`) : storagePickerNames;
    const entries: SandboxEntry[] = names.map((name) => ({
        name,
        relativePath: `${storagePickerDirectory.relativePath}/${name}`,
        kind: name.endsWith('.hds') ? 'FILE' : 'DIRECTORY',
        size: name.endsWith('.hds') ? 1024 : null,
    }));
    const pageSize = longList ? 45 : 16;
    return {
        storageMode: 'server',
        connectionMode: 'remote',
        supportsClientUploads: false,
        sandboxRoots: async () => [{ id: 'workspace', displayName: 'Yamaha images', writable: true }],
        sandboxDirectory: async (directory: DirectoryRef, cursor?: string) => ({
            directory,
            entries: cursor ? entries.slice(pageSize) : entries.slice(0, pageSize),
            truncated: !cursor,
            nextCursor: cursor ? null : 'next',
        }),
        inspectSandboxMediaSource: async () => null,
    } as unknown as ImageTransport;
}
