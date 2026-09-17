import { describe, expect, it, vi } from 'vitest';
import type { DirectoryListing, DirectoryRef, SandboxEntry } from '../../../lib/storageLocations';
import { readFilesystemImportTree } from './filesImportTree';

const directory: DirectoryRef = { rootId: 'host', relativePath: 'sources' };
const entry = (name: string, kind: 'FILE' | 'DIRECTORY' = 'FILE', parent = 'sources'): SandboxEntry => ({
    name,
    relativePath: parent ? `${parent}/${name}` : name,
    kind,
    size: kind === 'FILE' ? 0 : null,
});
const page = (entries: SandboxEntry[], reference = directory, nextCursor: string | null = null): DirectoryListing => ({
    directory: reference,
    entries,
    truncated: nextCursor !== null,
    nextCursor,
});
function setup() {
    const list = vi.fn<(directory: DirectoryRef, cursor?: string) => Promise<DirectoryListing>>();
    const abort = new AbortController();
    const active = vi.fn();
    const progress = vi.fn();
    return {
        list,
        abort,
        active,
        progress,
        run: () => readFilesystemImportTree(directory, list, abort.signal, active, progress),
    };
}

describe('Filesystem directory import traversal', () => {
    it('collects paginated contents with parents before children and keeps empty directories', async () => {
        const test = setup();
        test.list.mockImplementation(async (ref, cursor) => {
            if (ref.relativePath === 'sources')
                return cursor
                    ? page([entry('empty'), entry('sub', 'DIRECTORY')])
                    : page([entry('vacant', 'DIRECTORY')], ref, 'page2');
            return page(ref.relativePath.endsWith('/sub') ? [entry('data', 'FILE', ref.relativePath)] : [], ref);
        });
        const rows = await test.run();
        expect(rows.map((row) => [row.relativePath, row.directory])).toEqual([
            [['vacant'], true],
            [['empty'], false],
            [['sub'], true],
            [['sub', 'data'], false],
        ]);
        expect(rows[1]).toMatchObject({
            source: { kind: 'server-file', reference: { rootId: 'host', relativePath: 'sources/empty' } },
        });
        expect(rows[0]).not.toHaveProperty('source');
        expect(test.list.mock.calls).toEqual([
            [directory, undefined],
            [directory, 'page2'],
            [{ rootId: 'host', relativePath: 'sources/vacant' }, undefined],
            [{ rootId: 'host', relativePath: 'sources/sub' }, undefined],
        ]);
        expect(test.progress).toHaveBeenLastCalledWith('4 entries found');
    });
    it('accepts an empty directory and the workspace root without synthesizing a directory name', async () => {
        const test = setup();
        test.list.mockResolvedValue(page([]));
        expect(await test.run()).toEqual([]);
        const root = { rootId: 'host', relativePath: '' };
        test.list.mockResolvedValue(page([entry('empty', 'FILE', '')], root));
        const result = await readFilesystemImportTree(root, test.list, test.abort.signal, test.active, test.progress);
        expect(result[0].relativePath).toEqual(['empty']);
    });
    it.each([
        page([], { rootId: 'another', relativePath: 'sources' }),
        page([], { rootId: 'host', relativePath: 'elsewhere' }),
        page([{ ...entry('bad'), relativePath: 'outside/bad' }]),
        page([entry('..')]),
        page([entry('a/b')]),
        page([entry('a\\b')]),
        page([entry('')]),
        page([entry('bad\0name')]),
        { ...page([]), truncated: true },
        { ...page([entry('a')], directory, 'next'), truncated: false },
        page([], directory, 'next'),
    ])('rejects mismatched paths and incomplete pagination %#', async (listing) => {
        const test = setup();
        test.list.mockResolvedValue(listing);
        await expect(test.run()).rejects.toThrow();
        expect(test.list).toHaveBeenCalledOnce();
    });
    it('rejects duplicate entries across pages and repeated cursors rather than looping or omitting data', async () => {
        const test = setup();
        test.list
            .mockResolvedValueOnce(page([entry('same')], directory, 'next'))
            .mockResolvedValueOnce(page([entry('same')]));
        await expect(test.run()).rejects.toThrow('duplicate');
        const next = setup();
        next.list
            .mockResolvedValueOnce(page([entry('one')], directory, 'next'))
            .mockResolvedValueOnce(page([entry('two')], directory, 'next'));
        await expect(next.run()).rejects.toThrow('pagination');
        expect(next.list).toHaveBeenCalledTimes(2);
    });
    it('enforces the complete batch entry limit before descending', async () => {
        const test = setup();
        test.list.mockResolvedValue(
            page(Array.from({ length: 10001 }, (_, index) => entry(String(index), 'DIRECTORY'))),
        );
        await expect(test.run()).rejects.toThrow('10000');
        expect(test.list).toHaveBeenCalledOnce();
    });
    it('bounds cumulative retained path data', async () => {
        const test = setup();
        test.list.mockResolvedValue(
            page(Array.from({ length: 10000 }, (_, index) => entry(`${index}${'x'.repeat(512)}`))),
        );
        await expect(test.run()).rejects.toThrow('path data');
    });
    it('enforces the filesystem depth limit without recursive calls', async () => {
        const test = setup();
        test.list.mockImplementation(async (ref) => page([entry('a', 'DIRECTORY', ref.relativePath)], ref));
        await expect(test.run()).rejects.toThrow('depth limit');
        expect(test.list).toHaveBeenCalledTimes(1024);
    });
    it('does not return a partial selection after a child listing fails', async () => {
        const test = setup();
        test.list
            .mockResolvedValueOnce(page([entry('sub', 'DIRECTORY')]))
            .mockRejectedValueOnce(new Error('Directory changed'));
        await expect(test.run()).rejects.toThrow('Directory changed');
    });
    it('stops before further requests on cancellation or image navigation', async () => {
        const test = setup();
        test.abort.abort();
        await expect(test.run()).rejects.toThrow();
        expect(test.list).not.toHaveBeenCalled();
        const next = setup();
        next.list.mockImplementation(async () => {
            next.abort.abort();
            return page([entry('sub', 'DIRECTORY')]);
        });
        await expect(next.run()).rejects.toThrow();
        expect(next.list).toHaveBeenCalledOnce();
        const moved = setup();
        moved.list.mockImplementation(async () => {
            moved.active.mockImplementation(() => {
                throw new Error('Image closed');
            });
            return page([entry('sub', 'DIRECTORY')]);
        });
        await expect(moved.run()).rejects.toThrow('Image closed');
        expect(moved.list).toHaveBeenCalledOnce();
    });
});
