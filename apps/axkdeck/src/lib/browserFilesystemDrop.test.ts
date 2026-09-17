import { describe, expect, it, vi } from 'vitest';
import { captureBrowserFilesystemDrop } from './browserFilesystemDrop';

function file(name: string, bytes = ''): FileSystemEntry {
    return {
        name,
        isFile: true,
        isDirectory: false,
        file: (done: (file: File) => void) => done(new File([bytes], name)),
    } as unknown as FileSystemEntry;
}
function directory(name: string, batches: FileSystemEntry[][]): FileSystemEntry {
    let index = 0;
    return {
        name,
        isDirectory: true,
        isFile: false,
        createReader: () => ({
            readEntries: (done: (entries: FileSystemEntry[]) => void) => done(batches[index++] ?? []),
        }),
    } as unknown as FileSystemEntry;
}
function transfer(entries: FileSystemEntry[]): DataTransfer {
    return {
        items: entries.map((entry) => ({ kind: 'file', webkitGetAsEntry: () => entry })),
        files: [],
    } as unknown as DataTransfer;
}
const scan = async (entries: FileSystemEntry[], signal = new AbortController().signal) =>
    captureBrowserFilesystemDrop(transfer(entries))(signal, vi.fn());

describe('browser Files drop acquisition', () => {
    it('captures handles synchronously and keeps all paginated children and empty directories', async () => {
        const entry = directory('ROOT', [[file('ONE')], [directory('EMPTY', []), file('TWO', 'data')]]);
        const data = transfer([entry, file('ZERO')]);
        const captured = captureBrowserFilesystemDrop(data);
        Object.defineProperty(data, 'items', {
            get: () => {
                throw new Error('Expired drop store');
            },
        });
        const result = await captured(new AbortController().signal, vi.fn());
        expect(result.map((row) => [row.relativePath.join('/'), row.directory])).toEqual([
            ['ROOT', true],
            ['ROOT/ONE', false],
            ['ROOT/EMPTY', true],
            ['ROOT/TWO', false],
            ['ZERO', false],
        ]);
        const two = result.find((row) => row.relativePath.at(-1) === 'TWO')!;
        if (!two.directory) expect(two.source.size).toBe(4);
    });
    it('accepts raw file drops without extension filtering', async () => {
        const data = {
            items: [],
            files: [new File(['x'], 'unknown.foo'), new File([], 'empty')],
        } as unknown as DataTransfer;
        expect(
            (await captureBrowserFilesystemDrop(data)(new AbortController().signal, vi.fn())).map(
                (row) => row.relativePath,
            ),
        ).toEqual([['unknown.foo'], ['empty']]);
    });
    it('rejects cycles, unsafe names, duplicate paths and oversized collections', async () => {
        const loop = directory('LOOP', []);
        Object.assign(loop, {
            createReader: () => ({ readEntries: (done: (entries: FileSystemEntry[]) => void) => done([loop]) }),
        });
        await expect(scan([loop])).rejects.toThrow();
        await expect(scan([file('../escape')])).rejects.toThrow();
        await expect(scan([file('same'), file('same')])).rejects.toThrow(/duplicate/i);
        await expect(scan(Array.from({ length: 10001 }, (_, i) => file(String(i))))).rejects.toThrow(/10000/);
    });
    it('fails the complete scan on read errors and supports cancellation during an outstanding callback', async () => {
        const broken = {
            name: 'BROKEN',
            isDirectory: false,
            isFile: true,
            file: (_: unknown, fail: (error: Error) => void) => fail(new Error('Denied')),
        } as unknown as FileSystemEntry;
        await expect(scan([file('first'), broken])).rejects.toThrow('Denied');
        const pending = {
            name: 'PENDING',
            isDirectory: true,
            isFile: false,
            createReader: () => ({ readEntries: () => undefined }),
        } as unknown as FileSystemEntry;
        const abort = new AbortController();
        const promise = scan([pending], abort.signal);
        abort.abort();
        await expect(promise).rejects.toThrow(/abort/i);
    });
});
