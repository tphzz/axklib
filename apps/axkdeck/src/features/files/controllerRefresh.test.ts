import { describe, expect, it } from 'vitest';
import type { FilesystemEntry } from '../../lib/filesystem';
import { filesystemEntry } from '../../lib/testing/filesystem';
import { FilesController } from './controller.svelte';

function fixture() {
    const root = filesystemEntry({ id: 'root', kind: 'root', path: '', parentId: null, ancestorIds: [] });
    const folder = filesystemEntry({ id: 'folder', name: 'Folder', path: '/Folder' });
    const nested = filesystemEntry({
        id: 'nested',
        name: 'Nested',
        path: '/Folder/Nested',
        parentId: folder.id,
        ancestorIds: ['root', folder.id],
    });
    const file = filesystemEntry({
        id: 'file',
        name: 'File',
        path: '/Folder/Nested/File',
        kind: 'file',
        parentId: nested.id,
        ancestorIds: ['root', folder.id, nested.id],
    });
    const second = { ...root, id: 'second', rootId: 'second' };
    const other = { ...folder, id: 'other', rootId: second.id, parentId: second.id, ancestorIds: [second.id] };
    let entries = [root, folder, nested, file, second, other];
    let revision = 1;
    const source = {
        inspect: async (query: import('../../lib/filesystem').FilesystemQuery = {}) => {
            const matches = query.entryId
                ? entries.filter((entry) => entry.id === query.entryId)
                : query.parentId
                  ? entries.filter((entry) => entry.parentId === query.parentId)
                  : query.rootId
                    ? entries.filter((entry) => entry.rootId === query.rootId && entry.name.includes(query.query ?? ''))
                    : entries.filter((entry) => entry.kind === 'root');
            // Deliberately small pages exercise every restoration pagination path.
            const offset = query.offset ?? 0;
            return {
                revision,
                available: true,
                filesystemName: 'FAT16',
                deviceView: null,
                rootCapabilities: [],
                items: matches.slice(offset, offset + 1),
                totalCount: matches.length,
            };
        },
    };
    return {
        root,
        folder,
        nested,
        file,
        second,
        other,
        source,
        controller: new FilesController(source),
        mutate(change: (entries: FilesystemEntry[]) => FilesystemEntry[]) {
            entries = change(entries);
            revision++;
        },
    };
}

describe('Files navigation refresh', () => {
    it('restores a nested expanded branch, selection, paging and scroll using new directory identities', async () => {
        const f = fixture();
        await f.controller.initialize();
        await f.controller.toggle(f.folder);
        await f.controller.toggle(f.nested);
        f.controller.select(f.file);
        f.controller.scrollTop = 72;
        f.mutate((entries) =>
            entries
                .map((entry) => ({
                    ...entry,
                    id: entry.id === 'folder' ? 'moved' : entry.id,
                    parentId: entry.parentId === 'folder' ? 'moved' : entry.parentId,
                    ancestorIds: entry.ancestorIds.map((id) => (id === 'folder' ? 'moved' : id)),
                }))
                .concat({ ...f.folder, path: '/Different', name: 'Different' }),
        );
        await f.controller.initialize();
        expect(f.controller.expanded('folder')).toBe(false);
        expect(f.controller.expanded('moved')).toBe(true);
        expect(f.controller.expanded('nested')).toBe(true);
        expect(f.controller.selected?.id).toBe('file');
        expect(f.controller.rows.map((row) => row.entry.id)).toContain('file');
        expect(f.controller.scrollTop).toBe(72);
    });

    it('keeps latent expansion below a collapsed parent and restores it when reopened', async () => {
        const f = fixture();
        await f.controller.initialize();
        await f.controller.toggle(f.folder);
        await f.controller.toggle(f.nested);
        await f.controller.toggle(f.folder);
        f.mutate((entries) => entries);
        await f.controller.initialize();
        expect(f.controller.expanded('folder')).toBe(false);
        expect(f.controller.expanded('nested')).toBe(true);
        expect(f.controller.rows.map((row) => row.entry.id)).toEqual(['folder']);
        await f.controller.toggle(f.folder);
        expect(f.controller.rows.map((row) => row.entry.id)).toEqual(['folder', 'nested', 'file']);
    });

    it('retains inactive partition state even across two refreshes before switching back', async () => {
        const f = fixture();
        await f.controller.initialize();
        await f.controller.toggle(f.folder);
        await f.controller.chooseRoot(f.second.id);
        await f.controller.toggle(f.other);
        f.controller.scrollTop = 17;
        await f.controller.chooseRoot(f.root.id);
        f.mutate((entries) => entries);
        await f.controller.initialize();
        f.mutate((entries) => entries);
        await f.controller.initialize();
        expect(f.controller.expanded('folder')).toBe(true);
        await f.controller.chooseRoot(f.second.id);
        expect(f.controller.expanded('other')).toBe(true);
        expect(f.controller.scrollTop).toBe(17);
    });

    it('restores a search selection without expanding its collapsed ancestors', async () => {
        const f = fixture();
        await f.controller.initialize();
        await f.controller.search('File');
        f.controller.select(f.file);
        f.mutate((entries) => entries);
        await f.controller.initialize();
        expect(f.controller.query).toBe('File');
        expect(f.controller.selected?.id).toBe('file');
        expect(f.controller.expanded('folder')).toBe(false);
        expect(f.controller.expanded('nested')).toBe(false);
        await f.controller.search('');
        expect(f.controller.rows.map((row) => row.entry.id)).toEqual(['folder']);
    });

    it('drops deleted paths, does not expand new directories and resets for a new session', async () => {
        const f = fixture();
        await f.controller.initialize();
        await f.controller.toggle(f.folder);
        await f.controller.toggle(f.nested);
        f.mutate((entries) =>
            entries
                .filter((entry) => entry.id !== 'nested' && entry.id !== 'file')
                .concat({ ...f.nested, path: '/Folder/New', name: 'New' }),
        );
        await f.controller.initialize();
        expect(f.controller.expanded('folder')).toBe(true);
        expect(f.controller.expanded('nested')).toBe(false);
        const fresh = new FilesController(f.source);
        await fresh.initialize();
        expect(fresh.expanded('folder')).toBe(false);
    });

    it('restores loaded pages even when no entry is selected or expanded', async () => {
        const f = fixture();
        f.mutate((entries) => entries.concat({ ...f.folder, id: 'last', name: 'Last', path: '/Last' }));
        await f.controller.initialize();
        await f.controller.loadChildren('root');
        expect(f.controller.rows).toHaveLength(2);
        f.mutate((entries) => entries);
        await f.controller.initialize();
        expect(f.controller.rows).toHaveLength(2);
        expect(f.controller.selection).toEqual([]);
    });

    it('retains paged search results and scroll without opening their ancestors', async () => {
        const f = fixture();
        f.mutate((entries) => entries.concat({ ...f.file, id: 'file2', name: 'File2', path: '/Folder/Nested/File2' }));
        await f.controller.initialize();
        await f.controller.search('File');
        await f.controller.search('File', true);
        f.controller.scrollTop = 60;
        f.mutate((entries) => entries);
        await f.controller.initialize();
        expect(f.controller.rows.map((row) => row.entry.id)).toEqual(['file', 'file2']);
        expect(f.controller.scrollTop).toBe(60);
        expect(f.controller.expanded('folder')).toBe(false);
        expect(f.controller.selection).toEqual([]);
    });
});
