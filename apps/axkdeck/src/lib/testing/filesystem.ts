import type { FilesystemEntry, FilesystemRootCapabilities } from '../filesystem';

export const writableFilesRoot: FilesystemRootCapabilities = {
    rootId: 'root',
    createDirectory: true,
    putFile: true,
    deleteEntry: true,
    renameEntry: true,
    moveEntry: true,
    maximumNameBytes: 23,
    namePolicy: 'PRESERVE',
    namePattern: '^[ -~]{1,23}$',
    nameHint: 'Use 1-23 printable ASCII characters.',
    supportedImports: [],
};

export function filesystemEntry(overrides: Partial<FilesystemEntry> = {}): FilesystemEntry {
    return {
        id: 'folder',
        rootId: 'root',
        parentId: 'root',
        ancestorIds: ['root'],
        name: 'Documents',
        path: '/Documents',
        kind: 'directory',
        sizeBytes: null,
        childCount: 0,
        objectId: null,
        contentScopeId: null,
        interpretation: '',
        storage: '',
        issue: '',
        filesystemMetadata: false,
        rawAttributes: '',
        attributes: [],
        ...overrides,
    };
}
