import { beforeEach, expect, it, vi } from 'vitest';

const fs = vi.hoisted(() => ({ lstat: vi.fn(), open: vi.fn() }));
vi.mock('@tauri-apps/plugin-fs', () => ({ ...fs, SeekMode: { Start: 0 } }));
import { nativeFileSource, nativeRawFileSource } from './nativeFileSource';

const info = () => ({ isFile: true, isSymlink: false, size: 4, ino: 5, dev: 1, mtime: new Date(1000) });
let handle: {
    stat: ReturnType<typeof vi.fn>;
    seek: ReturnType<typeof vi.fn>;
    read: ReturnType<typeof vi.fn>;
    close: ReturnType<typeof vi.fn>;
};
beforeEach(() => {
    vi.resetAllMocks();
    fs.lstat.mockImplementation(async () => info());
    handle = {
        stat: vi.fn(async () => info()),
        seek: vi.fn(),
        read: vi.fn(async (bytes: Uint8Array) => {
            bytes.fill(42);
            return bytes.length;
        }),
        close: vi.fn(),
    };
    fs.open.mockResolvedValue(handle);
});

it('admits raw empty and extensionless files lazily without weakening typed admission', async () => {
    fs.lstat.mockResolvedValue({ ...info(), size: 0 });
    const source = await nativeRawFileSource('/drop/EMPTY');
    expect(source.size).toBe(0);
    expect(fs.open).not.toHaveBeenCalled();
    await expect(nativeFileSource('/drop/EMPTY', new Set(['wav']), 'audio/wav')).rejects.toThrow('Unsupported');
});

it('reads bounded chunks and closes handles on success and read failure', async () => {
    const source = await nativeRawFileSource('/drop/RAW');
    expect((await source.readChunk(1, 4)).size).toBe(3);
    expect(handle.seek).toHaveBeenCalledWith(1, 0);
    expect(handle.stat).toHaveBeenCalledTimes(2);
    expect(handle.close).toHaveBeenCalledOnce();
    handle.read.mockRejectedValue(new Error('read failed'));
    await expect(source.readChunk(0, 2)).rejects.toThrow('read failed');
    expect(handle.close).toHaveBeenCalledTimes(2);
});

it.each(['ino', 'mtime'] as const)('rejects same-size replacement or editing using %s', async (field) => {
    const source = await nativeRawFileSource('/drop/RAW');
    handle.stat.mockResolvedValue({ ...info(), [field]: field === 'ino' ? 6 : new Date(2000) });
    await expect(source.readChunk(0, 4)).rejects.toThrow('changed');
    expect(handle.read).not.toHaveBeenCalled();
    expect(handle.close).toHaveBeenCalledOnce();
});

it('rejects path symlink replacement and changes during read', async () => {
    const source = await nativeRawFileSource('/drop/RAW');
    fs.lstat.mockResolvedValueOnce({ ...info(), isSymlink: true });
    await expect(source.readChunk(0, 4)).rejects.toThrow('changed');
    expect(fs.open).not.toHaveBeenCalled();
    handle.stat.mockResolvedValueOnce(info()).mockResolvedValueOnce({ ...info(), mtime: new Date(2000) });
    await expect(source.readChunk(0, 4)).rejects.toThrow('changed');
    await expect(source.readChunk(-1, 2)).rejects.toThrow('range');
});

it('caps raw reads even when the server permits larger chunks', async () => {
    fs.lstat.mockResolvedValue({ ...info(), size: 64 * 1024 * 1024 });
    handle.stat.mockResolvedValue({ ...info(), size: 64 * 1024 * 1024 });
    const source = await nativeRawFileSource('/drop/RAW');
    expect((await source.readChunk(0, source.size)).size).toBe(8 * 1024 * 1024);
});
