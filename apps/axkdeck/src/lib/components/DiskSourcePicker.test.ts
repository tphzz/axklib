import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';
const mocks = vi.hoisted(() => ({ select: vi.fn(), finishImport: null as (() => void) | null }));
vi.mock('../nativeDiskSources', async () => {
    await new Promise<void>((resolve) => {
        mocks.finishImport = resolve;
    });
    return { selectNativeDiskSources: mocks.select };
});
import DiskSourcePicker from './DiskSourcePicker.svelte';
afterEach(() => {
    delete (window as Window & { __TAURI_INTERNALS__?: unknown }).__TAURI_INTERNALS__;
    vi.resetAllMocks();
});
describe.sequential('native disk picker lifetime', () => {
    it('does not open a native picker after its lazy module finishes loading into an unmounted dialog', async () => {
        Object.assign(window, { __TAURI_INTERNALS__: {} });
        mocks.select.mockResolvedValue([]);
        const onadd = vi.fn(),
            onerror = vi.fn();
        const view = render(DiskSourcePicker, { onadd, onerror });
        await fireEvent.click(screen.getByRole('button', { name: 'Add disks' }));
        await waitFor(() => expect(mocks.finishImport).not.toBeNull());
        await view.unmount();
        mocks.finishImport!();
        await vi.dynamicImportSettled();
        expect(mocks.select).not.toHaveBeenCalled();
        expect(onadd).not.toHaveBeenCalled();
        expect(onerror).not.toHaveBeenCalled();
    });
    it('uses native disk capabilities instead of a browser file input in a desktop window', async () => {
        Object.assign(window, { __TAURI_INTERNALS__: {} });
        const files = [{ name: 'first.img', type: 'application/x-raw-disk-image', size: 3, readChunk: vi.fn() }];
        mocks.select.mockResolvedValue(files);
        const onadd = vi.fn();
        const { container } = render(DiskSourcePicker, { onadd, onerror: vi.fn() });
        expect(container.querySelector('input')).toBeNull();
        await fireEvent.click(screen.getByRole('button', { name: 'Add disks' }));
        await waitFor(() => expect(onadd).toHaveBeenCalledWith(files));
    });
    it('does not add files to a later dialog after the original picker is unmounted', async () => {
        Object.assign(window, { __TAURI_INTERNALS__: {} });
        let finish!: (files: unknown[]) => void;
        mocks.select.mockImplementation(
            () =>
                new Promise((resolve) => {
                    finish = resolve;
                }),
        );
        const onadd = vi.fn(),
            onerror = vi.fn();
        const view = render(DiskSourcePicker, { onadd, onerror });
        await fireEvent.click(screen.getByRole('button', { name: 'Add disks' }));
        await waitFor(() => expect(mocks.select).toHaveBeenCalledOnce());
        await view.unmount();
        finish([{ name: 'late.img' }]);
        await Promise.resolve();
        await Promise.resolve();
        expect(onadd).not.toHaveBeenCalled();
        expect(onerror).not.toHaveBeenCalled();
    });
});
