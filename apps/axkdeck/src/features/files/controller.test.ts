import { describe, expect, it } from 'vitest';
import type { FilesystemAccess, FilesystemEntry, FilesystemPage } from '../../lib/filesystem';
import { FilesController } from './controller.svelte';

const entry = (id: string, parentId: string | null, kind: FilesystemEntry['kind']): FilesystemEntry => ({
    id,
    parentId,
    rootId: 'disk',
    ancestorIds: parentId ? ['disk'] : [],
    name: id,
    path: parentId ? `/${id}` : '',
    kind,
    sizeBytes: kind === 'file' ? 12 : null,
    childCount: kind === 'file' ? 0 : 1,
    objectId: null,
    contentScopeId: null,
    interpretation: '',
    storage: '',
    filesystemMetadata: false,
    rawAttributes: '',
    attributes: [],
    issue: '',
});
const root = entry('disk', null, 'root');
const file = entry('other-sampler-file', 'disk', 'file');
const page = (items: FilesystemEntry[]): FilesystemPage => ({
    revision: 1,
    available: true,
    filesystemName: 'Another filesystem',
    deviceView: null,
    items,
    totalCount: items.length,
    rootCapabilities: [],
});
const access: FilesystemAccess = {
    inspect: async (query = {}) => page(query.parentId || query.rootId ? [file] : [root]),
};

describe('FilesController', () => {
    it.each([2, 3])('restores renamed navigation only at the confirmed revision, received %s', async (revision) => {
        const folder = entry('folder', 'disk', 'directory');
        const child = {
            ...file,
            parentId: 'folder',
            path: '/folder/child',
            name: 'child',
            ancestorIds: ['disk', 'folder'],
        };
        let currentRevision = 1;
        const files = new FilesController({
            inspect: async (query = {}) => {
                const renamed = currentRevision > 1;
                const entries = [
                    root,
                    { ...folder, name: renamed ? 'Renamed' : 'folder', path: renamed ? '/Renamed' : '/folder' },
                    { ...child, path: renamed ? '/Renamed/child' : child.path },
                ];
                const items = query.entryId
                    ? entries.filter((value) => value.id === query.entryId)
                    : query.parentId
                      ? entries.filter((value) => value.parentId === query.parentId)
                      : query.rootId
                        ? entries.filter((value) => value.name.includes(query.query ?? ''))
                        : [root];
                return { ...page(items), revision: currentRevision };
            },
        });
        await files.initialize();
        await files.toggle(folder);
        files.select(child);
        files.scrollTop = 42;
        files.recordRename(1, folder, 'Renamed', 2);
        currentRevision = revision;
        await files.initialize();
        expect(files.scrollTop).toBe(42);
        if (revision === 2) {
            expect(files.selected?.path).toBe('/Renamed/child');
            expect(files.expanded('folder')).toBe(true);
        } else expect(files.selection).toEqual([]);
    });

    it('uses capabilities for the active root and replaces them after a refresh', async () => {
        const capability = {
            rootId: root.id,
            createDirectory: true,
            putFile: true,
            deleteEntry: true,
            renameEntry: true,
            maximumNameBytes: 23,
            namePattern: '^[ -~]{1,23}$',
            nameHint: 'Printable ASCII',
            supportedImports: [],
        };
        let writable = true;
        const files = new FilesController({
            inspect: async (query = {}) => ({
                ...(await access.inspect(query)),
                rootCapabilities: writable ? [capability] : [],
            }),
        });
        expect(files.capabilities).toBeNull();
        await files.initialize();
        expect(files.capabilities).toEqual(capability);
        writable = false;
        await files.initialize();
        expect(files.capabilities).toBeNull();
    });
    it('rejects an ambiguous object mapping', async () => {
        const files = new FilesController({ inspect: async () => page([file, { ...file, id: 'alias' }]) });
        expect(await files.lookup({ objectId: 'object' })).toBeNull();
    });
    it('reveals a paged descendant and collapses a selected child to its parent', async () => {
        const directory = entry('folder', 'disk', 'directory');
        const nested = {
            ...file,
            id: 'nested-file',
            path: '/folder/nested-file',
            parentId: directory.id,
            ancestorIds: ['disk', 'folder'],
        };
        const files = new FilesController({
            inspect: async (query = {}) => {
                if (query.parentId === 'folder') return page([nested]);
                if (query.parentId === 'disk') return { ...page(query.offset ? [directory] : [file]), totalCount: 2 };
                return page([root]);
            },
        });
        await files.initialize();
        expect(await files.reveal(nested)).toBe(true);
        expect(files.rows.map((row) => row.entry.id)).toEqual([file.id, 'folder', nested.id]);
        expect(files.selected?.parentId).toBe('folder');
        await files.toggle(directory);
        expect(files.selected?.id).toBe('folder');
    });
    it('does not claim a successful reveal when the target is missing', async () => {
        const files = new FilesController(access);
        await files.initialize();
        expect(await files.reveal({ ...file, id: 'missing' })).toBe(false);
        expect(files.revealSequence).toBe(0);
    });
    it('retains selection and scroll position across a revision refresh', async () => {
        const files = new FilesController({
            inspect: async (query = {}) => (query.entryId ? page([file]) : access.inspect(query)),
        });
        await files.initialize();
        files.select(file);
        files.scrollTop = 42;
        await files.initialize();
        expect(files.selected?.id).toBe(file.id);
        expect(files.scrollTop).toBe(42);
    });
    it('restores the selected path rather than a reused entry identity after a mutation', async () => {
        let revision = 1;
        const replacement = { ...file, id: 'new-identity' };
        const reused = { ...file, name: 'Different', path: '/Different' };
        const files = new FilesController({
            inspect: async (query = {}) => {
                let items = [root];
                if (query.entryId) items = [revision === 1 ? file : reused];
                else if (query.rootId) items = [replacement];
                else if (query.parentId) items = revision === 1 ? [file] : [reused, replacement];
                return { ...page(items), revision };
            },
        });
        await files.initialize();
        files.select(file);
        revision = 2;
        await files.initialize();
        expect(files.selected?.id).toBe('new-identity');
        expect(files.selected?.path).toBe(file.path);
    });
    it('browses a files-only backend without Yamaha types or transport', async () => {
        const files = new FilesController(access);
        await files.initialize();
        expect(files.roots).toEqual([root]);
        expect(files.deviceView).toBeNull();
        expect(files.rows.map((row) => row.entry.id)).toEqual([file.id]);
    });
    it('searches the complete root through the driver rather than filtering loaded rows', async () => {
        const files = new FilesController(access);
        await files.initialize();
        await files.search('other');
        expect(files.rows[0]?.entry.id).toBe(file.id);
        expect(files.query).toBe('other');
    });
    it('ignores a late response after disposal', async () => {
        let resolve!: (value: FilesystemPage) => void;
        const files = new FilesController({
            inspect: () =>
                new Promise((done) => {
                    resolve = done;
                }),
        });
        const pending = files.initialize();
        files.dispose();
        resolve(page([root]));
        await pending;
        expect(files.roots).toEqual([]);
    });
});
