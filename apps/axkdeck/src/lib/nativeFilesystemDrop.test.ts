import { beforeEach, expect, it, vi } from 'vitest';
const mocks = vi.hoisted(() => ({ invoke: vi.fn(), lstat: vi.fn(), open: vi.fn() }));
vi.mock('@tauri-apps/api/core', () => ({ invoke: mocks.invoke }));
vi.mock('@tauri-apps/plugin-fs', () => ({ lstat: mocks.lstat, open: mocks.open, SeekMode: { Start: 0 } }));
import { nativeFilesystemDrop } from './nativeFilesystemDrop';

const file = { isFile: true, isDirectory: false, isSymlink: false, size: 0, mtime: new Date(1000) };
const directory = { ...file, isFile: false, isDirectory: true };
beforeEach(() => {
    vi.resetAllMocks();
    mocks.lstat.mockImplementation(async (path: string) => (path.endsWith('RAW') ? file : directory));
    mocks.invoke.mockImplementation(async (_command, { path }) => (path === '/DROP' ? ['EMPTY', 'RAW'] : []));
});
const run = (paths: string[], signal = new AbortController().signal) => nativeFilesystemDrop(paths)(signal, vi.fn());

it('retains dropped folder names, empty children and raw extensionless files without reading payloads', async () => {
    const rows = await run(['/DROP']);
    expect(rows.map((row) => [row.relativePath, row.directory])).toEqual([
        [['DROP'], true],
        [['DROP', 'EMPTY'], true],
        [['DROP', 'RAW'], false],
    ]);
    expect(mocks.open).not.toHaveBeenCalled();
    expect(mocks.invoke).toHaveBeenCalledWith('read_native_drop_directory', { path: '/DROP', limit: 9999 });
});

it('rejects links, unsafe names, duplicates and excessive entries before upload', async () => {
    mocks.lstat.mockResolvedValueOnce({ ...directory, isSymlink: true });
    await expect(run(['/DROP'])).rejects.toThrow('regular');
    mocks.invoke.mockResolvedValueOnce(['../escape']);
    await expect(run(['/DROP'])).rejects.toThrow('path');
    mocks.invoke.mockResolvedValueOnce(['RAW', 'RAW']);
    await expect(run(['/DROP'])).rejects.toThrow('Duplicate');
    await expect(run(Array(10001).fill('/RAW'))).rejects.toThrow('10000');
});

it('fails the complete scan on a read error, cancellation or changed directory', async () => {
    mocks.invoke.mockRejectedValueOnce(new Error('denied'));
    await expect(run(['/DROP'])).rejects.toThrow('denied');
    const abort = new AbortController();
    mocks.invoke.mockImplementationOnce(async () => {
        abort.abort();
        return ['RAW'];
    });
    await expect(run(['/DROP'], abort.signal)).rejects.toThrow();
    mocks.lstat.mockResolvedValueOnce(directory).mockResolvedValueOnce({ ...directory, mtime: new Date(2000) });
    mocks.invoke.mockResolvedValueOnce([]);
    await expect(run(['/DROP'])).rejects.toThrow('changed');
    expect(mocks.open).not.toHaveBeenCalled();
});

it('rejects Windows reparse points, oversized files and aggregate payloads', async () => {
    mocks.lstat.mockResolvedValueOnce({ ...directory, fileAttributes: 0x400 });
    await expect(run(['/DROP'])).rejects.toThrow('regular');
    mocks.lstat.mockResolvedValueOnce({ ...file, size: 0x100000000 });
    await expect(run(['/RAW'])).rejects.toThrow('size limit');
    mocks.lstat.mockResolvedValue({ ...file, size: 0xffffffff });
    await expect(run(['/ONE', '/TWO', '/THREE'])).rejects.toThrow('aggregate');
    expect(mocks.open).not.toHaveBeenCalled();
});
