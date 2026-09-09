import { describe, expect, it } from 'vitest';
import { FilesController } from './controller.svelte';
import { filesystemEntry } from '../../lib/testing/filesystem';

async function setup() {
    const root = filesystemEntry({ id: 'root', kind: 'root', parentId: null, path: '', ancestorIds: [] });
    const folder = filesystemEntry();
    const entries = ['a', 'b', 'c'].map((name) =>
        filesystemEntry({
            id: name,
            name,
            path: `/${name}`,
            kind: 'file',
        }),
    );
    const nested = filesystemEntry({
        id: 'nested',
        name: 'Nested',
        path: '/Documents/Nested',
        kind: 'file',
        parentId: folder.id,
        ancestorIds: ['root', folder.id],
    });
    let revision = 1;
    let removed = '';
    const current = () =>
        [root, folder, nested, ...entries]
            .filter((entry) => entry.name !== removed)
            .map((entry) => ({ ...entry, id: revision === 1 || entry.id === 'root' ? entry.id : `new-${entry.id}` }));
    const controller = new FilesController({
        inspect: async (query = {}) => {
            const items = current().filter((entry) =>
                query.entryId
                    ? entry.id === query.entryId
                    : query.rootId
                      ? entry.name.includes(query.query ?? '') && entry.kind !== 'root'
                      : query.parentId
                        ? entry.parentId === query.parentId
                        : entry.kind === 'root',
            );
            return {
                revision,
                available: true,
                deviceView: null,
                filesystemName: 'Test',
                items,
                totalCount: items.length,
                rootCapabilities: [],
            };
        },
    });
    await controller.initialize();
    return {
        controller,
        entries,
        folder,
        nested,
        change: () => {
            revision = 2;
            removed = 'b';
        },
    };
}

describe('Files selection', () => {
    it('restores selections from later search pages without hiding them', async () => {
        const root = filesystemEntry({ id: 'root', kind: 'root', parentId: null, path: '', ancestorIds: [] });
        const entries = ['Item 1', 'Item 2'].map((name) =>
            filesystemEntry({ id: name, name, path: `/${name}`, kind: 'file' }),
        );
        const files = new FilesController({
            inspect: async (query = {}) => {
                const all = query.entryId
                    ? entries.filter((entry) => entry.id === query.entryId)
                    : query.rootId || query.parentId
                      ? entries
                      : [root];
                return {
                    revision: 1,
                    available: true,
                    deviceView: null,
                    filesystemName: 'Test',
                    items: query.rootId ? all.slice(query.offset ?? 0, (query.offset ?? 0) + 1) : all,
                    totalCount: all.length,
                    rootCapabilities: [],
                };
            },
        });
        await files.initialize();
        await files.search('Item');
        await files.search('Item', true);
        files.select(entries[0], 'all');
        await files.initialize();
        expect(files.query).toBe('Item');
        expect(files.rows).toHaveLength(2);
        expect(files.selection.map((entry) => entry.id)).toEqual(['Item 1', 'Item 2']);
    });
    it('keeps the range anchor when selection survives refresh', async () => {
        const {
            controller: files,
            entries: [a, b, c],
        } = await setup();
        files.select(a);
        files.select(c, 'range');
        await files.initialize();
        files.select(b, 'range');
        expect(files.selection.map((entry) => entry.id)).toEqual(['a', 'b']);
    });

    it('isolates selections by root and rejects an entry from another root', async () => {
        const roots = ['one', 'two'].map((id) =>
            filesystemEntry({ id, rootId: id, kind: 'root', parentId: null, path: '', ancestorIds: [] }),
        );
        const entries = roots.map((root) =>
            filesystemEntry({
                id: `${root.id}-file`,
                rootId: root.id,
                parentId: root.id,
                ancestorIds: [root.id],
                kind: 'file',
            }),
        );
        const files = new FilesController({
            inspect: async (query = {}) => {
                const items = query.parentId ? entries.filter((entry) => entry.parentId === query.parentId) : roots;
                return {
                    revision: 1,
                    available: true,
                    deviceView: null,
                    filesystemName: 'Test',
                    items,
                    totalCount: items.length,
                    rootCapabilities: [],
                };
            },
        });
        await files.initialize();
        files.select(entries[0]);
        await files.chooseRoot('two');
        files.select(entries[0]);
        expect(files.selection).toEqual([]);
        files.select(entries[1]);
        await files.chooseRoot('one');
        expect(files.selection.map((entry) => entry.id)).toEqual(['one-file']);
    });
    it('supports toggles, stable range anchors, additive ranges and visible select-all', async () => {
        const {
            controller: files,
            entries: [a, b, c],
            folder,
        } = await setup();
        files.select(a);
        files.select(c, 'toggle');
        expect(files.selection.map((entry) => entry.id)).toEqual(['a', 'c']);
        files.select(b, 'range');
        expect(files.selection.map((entry) => entry.id)).toEqual(['b', 'c']);
        files.select(a, 'add-range');
        expect(files.selection.map((entry) => entry.id)).toEqual(['a', 'b', 'c']);
        files.select(b, 'range');
        expect(files.selection.map((entry) => entry.id)).toEqual(['b', 'c']);
        files.select(c, 'toggle');
        expect(files.selection.map((entry) => entry.id)).toEqual(['b']);
        files.select(a, 'all');
        expect(files.selection.map((entry) => entry.id)).toEqual([folder.id, 'a', 'b', 'c']);
    });

    it('moves focus without changing selection and preserves the batch for its context menu', async () => {
        const {
            controller: files,
            entries: [a, b, c],
        } = await setup();
        files.select(a);
        files.select(b, 'toggle');
        files.focus(c);
        expect(files.focused?.id).toBe('c');
        expect(files.selected?.id).toBe('a');
        expect(files.selection.map((entry) => entry.id)).toEqual(['a', 'b']);
        files.selectForContext(a);
        expect(files.selection.map((entry) => entry.id)).toEqual(['a', 'b']);
        files.selectForContext(c);
        expect(files.selection.map((entry) => entry.id)).toEqual(['c']);
    });

    it('removes hidden descendants from selection when collapsing, retaining unrelated selections', async () => {
        const {
            controller: files,
            entries: [a],
            folder,
            nested,
        } = await setup();
        await files.toggle(folder);
        files.select(a);
        files.select(nested, 'toggle');
        await files.toggle(folder);
        expect(files.selection.map((entry) => entry.id)).toEqual([folder.id, 'a']);
        expect(files.selected?.id).toBe(folder.id);
    });

    it('clears hidden selections on changed search, but retains them while paging the same query', async () => {
        const {
            controller: files,
            entries: [a, b],
        } = await setup();
        files.select(a);
        files.select(b, 'toggle');
        await files.search('a');
        expect(files.selection).toEqual([]);
        files.select(a);
        await files.search('a', true);
        expect(files.selection.map((entry) => entry.id)).toEqual(['a']);
        await files.search('');
        expect(files.selection).toEqual([]);
    });

    it('restores surviving selection paths on refresh and drops removed entries', async () => {
        const {
            controller: files,
            entries: [a, b, c],
            change,
        } = await setup();
        files.select(a);
        files.select(b, 'toggle');
        files.select(c, 'toggle');
        change();
        await files.initialize();
        expect(files.selection.map((entry) => entry.id)).toEqual(['new-a', 'new-c']);
        expect(files.selected?.id).toBe('new-c');
    });
});
